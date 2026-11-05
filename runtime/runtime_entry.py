#!/usr/bin/env python3
"""Entrypoint shared by source, installed, and packaged runtimes."""

import argparse
import hashlib
import json
import os
from pathlib import Path


def command(root, arguments):
    """Return an argv, leaving configuration overlay resolution to Cyber."""
    root = Path(root).resolve()
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--dag")
    selection.add_argument("--launch")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    options = parser.parse_args(arguments)
    extra = options.arguments
    if extra[:1] == ["--"]:
        extra = extra[1:]
    dag, launch = options.dag, options.launch
    if not dag and not launch:
        manifest = root / "conf/runtime.json"
        defaults = json.loads(manifest.read_text()) if manifest.is_file() else {}
        dag, launch = defaults.get("default_dag"), defaults.get("default_launch")
    if bool(dag) == bool(launch):
        parser.error("select exactly one --dag or --launch (or a runtime default)")
    if launch:
        if extra:
            parser.error("extra mainboard arguments cannot be used with --launch")
        return [
            "python3",
            str(root / "bin/cyber_launch.py"),
            "start",
            str(root / launch),
        ]
    return [str(root / "bin/mainboard"), "-d", str(root / dag), *extra]


def environment(root, inherited=None):
    """Use one work/config root, never the caller's Bazel library cache."""
    root = Path(root).resolve()
    env = dict(os.environ if inherited is None else inherited)
    env["APOLLO_ROOT_DIR"] = env["CYBER_PATH"] = str(root)
    env["WHEELOS_RUNTIME_ROOT"] = str(root)
    env.setdefault("WHEELOS_ASSET_ROOT", str(root / "assets"))
    env["LD_LIBRARY_PATH"] = str(root / "lib")
    if env.get("WHEELOS_RUNTIME_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] += ":" + env["WHEELOS_RUNTIME_LIBRARY_PATH"]
    env["PATH"] = str(root / "bin/commands") + os.pathsep + env.get("PATH", "")
    python = root / "python"
    paths = [str(python)]
    if python.is_dir():
        paths.extend(str(path) for path in sorted(python.iterdir()) if path.is_dir())
    env["PYTHONPATH"] = os.pathsep.join(paths)
    if "GLOG_log_dir" not in env:
        env.setdefault("GLOG_logtostderr", "1")
    # A caller's Bazel runfiles must not redirect installed Python programs.
    for key in ("RUNFILES_DIR", "RUNFILES_MANIFEST_FILE", "JAVA_RUNFILES"):
        env.pop(key, None)
    return env


def _required_path(root, key):
    if (
        not key
        or key.startswith("/")
        or "\\" in key
        or any(part in ("", ".", "..") for part in key.split("/"))
    ):
        raise ValueError(f"unsafe required runtime key: {key!r}")
    path = (root / key).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError(f"required runtime input is missing or escapes root: {key}")
    return path


def validate_required(root, env):
    """Validate reviewed deployment inputs before starting any components."""
    manifest = Path(root) / "conf/runtime.json"
    requirements = json.loads(manifest.read_text()) if manifest.is_file() else {}
    configs = requirements.get("required_configs", [])
    assets = requirements.get("required_assets", {})
    for keys, variable, default in (
        (configs, "WHEELOS_CONFIG_ROOT", None),
        (assets, "WHEELOS_ASSET_ROOT", str(Path(root) / "assets")),
    ):
        if not keys:
            continue
        configured = env.get(variable, default)
        if not configured or not Path(configured).is_absolute():
            raise ValueError(f"{variable} must be an explicit absolute directory")
        selected = Path(configured).resolve()
        if not selected.is_dir():
            raise ValueError(f"{variable} is not a directory: {selected}")
        for key in keys:
            path = _required_path(selected, key)
            if variable == "WHEELOS_ASSET_ROOT":
                digest = hashlib.sha256()
                with path.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(block)
                if digest.hexdigest() != assets[key].lower():
                    raise ValueError(f"required runtime asset SHA-256 mismatch: {key}")


def main():
    import sys

    root = Path(__file__).resolve().parents[2]
    arguments = command(root, sys.argv[1:])
    env = environment(root)
    validate_required(root, env)
    os.chdir(root)
    os.execvpe(arguments[0], arguments, env)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def repo_root() -> Path:
    test_srcdir = os.environ.get("TEST_SRCDIR")
    test_workspace = os.environ.get("TEST_WORKSPACE")
    if test_srcdir and test_workspace:
        return Path(test_srcdir) / test_workspace
    return Path(__file__).resolve().parents[5]


def example_env(root: Path) -> dict[str, str]:
    env = os.environ.copy()
    log_dir = root / "data" / "log"
    log_dir.mkdir(parents=True, exist_ok=True)
    path_entries = [str(root), *sys.path]
    existing_pythonpath = env.get("PYTHONPATH")
    if existing_pythonpath:
        path_entries.append(existing_pythonpath)
    env["PYTHONPATH"] = os.pathsep.join(path_entries)
    env["CYBER_PATH"] = str(root / "cyber")
    env["CYBER_DOMAIN_ID"] = "80"
    env["CYBER_IP"] = "127.0.0.1"
    env["GLOG_log_dir"] = str(log_dir)
    env["GLOG_alsologtostderr"] = "0"
    env["GLOG_colorlogtostderr"] = "1"
    env["GLOG_minloglevel"] = "0"
    env["TERMINFO"] = "/lib/terminfo/"
    env["sysmo_start"] = "0"
    return env


def run_ok(root: Path, relative_script: str, timeout_s: int = 30) -> None:
    script = root / relative_script
    result = subprocess.run(
        [sys.executable, str(script)],
        cwd=root,
        env=example_env(root),
        text=True,
        capture_output=True,
        timeout=timeout_s,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"{relative_script} failed with exit code {result.returncode}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )


def run_until_timeout(root: Path, relative_script: str, timeout_s: int = 5) -> None:
    script = root / relative_script
    try:
        result = subprocess.run(
            [sys.executable, str(script)],
            cwd=root,
            env=example_env(root),
            text=True,
            capture_output=True,
            timeout=timeout_s,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return
    raise AssertionError(
        f"{relative_script} exited early with code {result.returncode}\n"
        f"stdout:\n{result.stdout}\n"
        f"stderr:\n{result.stderr}"
    )


def main() -> None:
    root = repo_root()

    for script in [
        "cyber/python/cyber_py3/examples/time.py",
        "cyber/python/cyber_py3/examples/record.py",
        "cyber/python/cyber_py3/examples/timer.py",
        "cyber/python/cyber_py3/examples/parameter.py",
    ]:
        run_ok(root, script)

    for script in [
        "cyber/python/cyber_py3/examples/talker.py",
        "cyber/python/cyber_py3/examples/listener.py",
        "cyber/python/cyber_py3/examples/service.py",
        "cyber/python/cyber_py3/examples/client.py",
    ]:
        run_until_timeout(root, script)


if __name__ == "__main__":
    main()

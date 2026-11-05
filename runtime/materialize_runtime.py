#!/usr/bin/env python3
"""Copy only declared runtime inputs into a relocatable installation."""

import argparse
import hashlib
import json
import posixpath
import shutil
from pathlib import Path, PurePosixPath

COMMANDS = ("mainboard", "cyber_launch", "cyber_recorder", "cyber_monitor")
DATA_ROOTS = ("conf", "dag", "launch", "assets", "models")


def _digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.digest()


def safe_destination(value):
    """Reject ambiguous or escaping installation keys before touching output."""
    if (
        not value
        or "\\" in value
        or "\x00" in value
        or value.startswith("/")
        or any(part in ("", ".", "..") for part in value.rstrip("/").split("/"))
    ):
        raise ValueError(f"unsafe runtime destination: {value!r}")
    return PurePosixPath(value)


def _wrapper(name):
    target = "cyber_launch.py" if name == "cyber_launch" else name
    return f"""#!/usr/bin/env python3
import os
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "share/core"))
from runtime_entry import environment, validate_required
env = environment(root)
target = root / "bin/{target}"
if {name!r} in ("mainboard", "cyber_launch"):
    validate_required(root, env)
    os.chdir(root)
argv = [str(target), *sys.argv[1:]]
if {name!r} == "cyber_launch":
    argv.insert(0, "python3")
os.execvpe(argv[0], argv, env)
"""


def _module_wrapper(destination, interpreter=None):
    python = ""
    if interpreter:
        python = f"""
interpreter = root / {interpreter!r}
env["PYTHONHOME"] = str(interpreter.parent.parent)
os.execve(str(interpreter), [str(interpreter), target, *sys.argv[1:]], env)
"""
    return f"""#!/usr/bin/env python3
import os
from pathlib import Path
import sys
depth = {len(PurePosixPath(destination).parts) - 1}
root = Path(__file__).resolve().parents[depth]
sys.path.insert(0, str(root / "share/core"))
from runtime_entry import environment
env = environment(root)
env["RUNFILES_DIR"] = str(root / {destination + ".runfiles"!r})
target = str(root / {destination + ".real"!r})
{python}
os.execve(target, [target, *sys.argv[1:]], env)
"""


SETUP = """#!/usr/bin/env bash
_wheelos_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
export APOLLO_ROOT_DIR="$_wheelos_root" CYBER_PATH="$_wheelos_root"
export WHEELOS_RUNTIME_ROOT="$_wheelos_root"
export WHEELOS_ASSET_ROOT="${WHEELOS_ASSET_ROOT-$_wheelos_root/assets}"
export LD_LIBRARY_PATH="$_wheelos_root/lib"
if [[ -n "${WHEELOS_RUNTIME_LIBRARY_PATH:-}" ]]; then
  export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$WHEELOS_RUNTIME_LIBRARY_PATH"
fi
export PYTHONPATH="$_wheelos_root/python"
for _wheelos_python in "$_wheelos_root"/python/*; do
  if [[ -d "$_wheelos_python" ]]; then
    export PYTHONPATH="$PYTHONPATH:$_wheelos_python"
  fi
done
export PATH="$_wheelos_root/bin/commands:$PATH"
if [[ ! -v GLOG_log_dir ]]; then
  export GLOG_logtostderr="${GLOG_logtostderr-1}"
fi
unset RUNFILES_DIR RUNFILES_MANIFEST_FILE JAVA_RUNFILES
unset _wheelos_root _wheelos_python
"""


def materialize(manifest, root, mode_manifest=None):
    """Validate all contributions, then copy files (not source-tree symlinks)."""
    root = Path(root).absolute()
    modules = manifest.get("modules", [])
    if len(set(modules)) != len(modules):
        raise ValueError("duplicate runtime module name")
    for key in (
        modules
        + manifest.get("required_configs", [])
        + list(manifest.get("required_assets", {}))
    ):
        safe_destination(key)
    for key in ("default_dag", "default_launch"):
        if manifest.get(key):
            safe_destination(manifest[key])
    for digest in manifest.get("required_assets", {}).values():
        if len(digest) != 64 or any(
            char not in "0123456789abcdefABCDEF" for char in digest
        ):
            raise ValueError("required asset digest must be SHA-256 hex")
    generated = {
        "conf/runtime.json": (
            json.dumps(
                {
                    "modules": manifest.get("modules", []),
                    "default_dag": manifest.get("default_dag", ""),
                    "default_launch": manifest.get("default_launch", ""),
                    "required_assets": manifest.get("required_assets", {}),
                    "required_configs": manifest.get("required_configs", []),
                },
                indent=2,
            )
            + "\n"
        ),
        "bin/setup.bash": SETUP,
        "bin/run": (
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            "import sys\n"
            "root = Path(__file__).resolve().parents[1]\n"
            'sys.path.insert(0, str(root / "share/core"))\n'
            "from runtime_entry import main\n"
            "main()\n"
        ),
        "share/core/runtime_entry.py": Path(__file__)
        .with_name("runtime_entry.py")
        .read_text(),
    }
    if manifest.get("default_dag") and manifest.get("default_launch"):
        raise ValueError("default_dag and default_launch are mutually exclusive")
    for name in COMMANDS:
        generated[f"bin/commands/{name}"] = _wrapper(name)
    executables = manifest.get("executables", [])
    for destination in executables:
        safe_destination(destination)
    copies = {}
    repo_mappings = {}
    interpreters = {}
    declared = {}
    for source, destination in manifest.get("files", []):
        source = Path(source)
        if destination.endswith("/") and not source.is_dir():
            destination += source.name
        declared[str(safe_destination(destination))] = source

    def add(source, destination):
        key = str(safe_destination(destination))
        source = Path(source)
        link_key = key
        seen = set()
        while source.is_symlink() and (
            not source.exists() or (link_key == key and ".venv/bin/python" in key)
        ):
            if source in seen:
                raise ValueError(f"cyclic declared runtime symlink: {source}")
            seen.add(source)
            link = source.readlink()
            if link.is_absolute():
                raise ValueError(f"undeclared runtime symlink target: {source}")
            link_key = str(
                safe_destination(
                    posixpath.normpath(str(PurePosixPath(link_key).parent / str(link)))
                )
            )
            if link_key not in declared:
                raise FileNotFoundError(
                    f"required runtime symlink target is not declared: {link_key}"
                )
            source = declared[link_key]
        if ".venv/bin/python" in key and link_key != key:
            # Copies lose the interpreter symlink Python uses to find stdlib.
            for executable in executables:
                if key.startswith(executable + ".runfiles/"):
                    interpreters[executable] = link_key
        if not source.exists():
            raise FileNotFoundError(f"required runtime input is missing: {source}")
        if source.is_dir():
            for child in sorted(source.rglob("*")):
                if child.is_file():
                    add(child, str(PurePosixPath(key) / child.relative_to(source)))
                elif not child.is_dir():
                    raise ValueError(f"unsupported runtime input: {child}")
            return
        if not source.is_file():
            raise ValueError(f"unsupported runtime input: {source}")
        if key in generated:
            raise ValueError(f"reserved core runtime destination: {key}")
        if key.endswith("/_repo_mapping"):
            mappings = repo_mappings.setdefault(key, {})
            for line in source.read_text().splitlines():
                fields = line.split(",")
                if len(fields) not in (2, 3) or not fields[0] or not fields[1]:
                    raise ValueError(
                        f"invalid repository mapping in {source}: {line!r}"
                    )
                mapping_key = (fields[0], fields[1])
                canonical_repo = fields[2] if len(fields) == 3 else fields[1]
                previous = mappings.get(mapping_key)
                if previous and previous[0] != canonical_repo:
                    raise ValueError(
                        "conflicting repository mappings for "
                        f"{mapping_key[0]},{mapping_key[1]}"
                    )
                if previous is None or (
                    len(fields) == 3 and len(previous[1].split(",")) == 2
                ):
                    mappings[mapping_key] = (canonical_repo, line)
            return
        if key in copies:
            previous = copies[key]
            content_conflict = previous != source and _digest(previous) != _digest(
                source
            )
            mode_conflict = key.split("/", 1)[0] not in DATA_ROOTS and bool(
                previous.stat().st_mode & 0o111
            ) != bool(source.stat().st_mode & 0o111)
            if content_conflict or mode_conflict:
                raise ValueError(f"conflicting runtime contents at {key}")
        else:
            copies[key] = source

    for source, destination in manifest.get("files", []):
        if destination.endswith("/") and not Path(source).is_dir():
            destination += Path(source).name
        add(source, destination)
    for item in manifest.get("libraries", []):
        source, destination = (
            item if isinstance(item, list) else (item, "lib/" + Path(item).name)
        )
        add(source, destination)
    for destination in executables:
        if destination not in copies:
            raise FileNotFoundError(f"required runtime executable: {destination}")
        real = destination + ".real"
        if real in copies or real in generated:
            raise ValueError(f"reserved runtime executable destination: {real}")
        copies[real] = copies.pop(destination)
        generated[destination] = _module_wrapper(
            destination, interpreters.get(destination)
        )
    keys = set(copies) | set(repo_mappings) | set(generated)
    for key in keys:
        for parent in PurePosixPath(key).parents:
            if str(parent) in keys:
                raise ValueError(f"runtime file/directory conflict: {key}")
        target = root / key
        if not target.resolve().is_relative_to(root.resolve()):
            raise ValueError(f"runtime destination escapes root: {key}")
    root.mkdir(parents=True, exist_ok=True)
    for directory in ("assets/models", "lib", "python"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    for key, source in copies.items():
        target = root / key
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        data = key.split("/", 1)[0] in DATA_ROOTS
        target.chmod(0o755 if not data and source.stat().st_mode & 0o111 else 0o644)
    for key, mappings in repo_mappings.items():
        target = root / key
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(
            "\n".join(sorted(value[1] for value in mappings.values())) + "\n"
            if mappings
            else ""
        )
        target.chmod(0o644)
    for key, contents in generated.items():
        target = root / key
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(contents)
        target.chmod(0o755 if key.startswith("bin/") else 0o644)
    for name in ("mainboard", "cyber_recorder", "cyber_monitor"):
        target = root / "bin" / name
        if target.is_file():
            target.chmod(target.stat().st_mode | 0o111)
    if mode_manifest is not None:
        Path(mode_manifest).write_text(
            json.dumps(
                [key for key in sorted(keys) if (root / key).stat().st_mode & 0o111]
            )
        )


def package_trees(root, executable_tree, data_tree, mode_manifest=None):
    """Partition modes because rules_pkg installs each tree with one mode."""
    root = Path(root)
    if not root.is_dir():
        raise FileNotFoundError(f"required runtime tree is missing: {root}")
    executable_tree = Path(executable_tree)
    data_tree = Path(data_tree)
    executable_tree.mkdir(parents=True, exist_ok=True)
    data_tree.mkdir(parents=True, exist_ok=True)
    executable_keys = (
        set(json.loads(Path(mode_manifest).read_text()))
        if mode_manifest is not None
        else None
    )
    for source in sorted(root.rglob("*")):
        relative = source.relative_to(root)
        if source.is_dir():
            (data_tree / relative).mkdir(parents=True, exist_ok=True)
            continue
        executable = (
            relative.as_posix() in executable_keys
            if executable_keys is not None
            else bool(source.stat().st_mode & 0o111)
        )
        destination = (executable_tree if executable else data_tree) / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(0o755 if executable else 0o644)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", action="store_true")
    parser.add_argument("--modes", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("data_tree", type=Path, nargs="?")
    args = parser.parse_args()
    if args.package:
        if args.data_tree is None:
            parser.error("--package requires source, executable tree and data tree")
        package_trees(args.source, args.destination, args.data_tree, args.modes)
    else:
        if args.data_tree is not None:
            parser.error("unexpected third destination")
        materialize(json.loads(args.source.read_text()), args.destination, args.modes)


if __name__ == "__main__":
    main()

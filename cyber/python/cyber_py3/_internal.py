#!/usr/bin/env python3

import importlib.util
import os
import sys
from pathlib import Path


def _extension_directories():
    runfiles_dir = os.environ.get("RUNFILES_DIR")
    if runfiles_dir:
        root = Path(runfiles_dir)
        if root.is_dir():
            yield root / "cyber/python/internal"
            for workspace in root.iterdir():
                if workspace.is_dir():
                    yield workspace / "cyber/python/internal"

    package_dir = Path(__file__).resolve().parent
    yield package_dir / "internal"
    yield package_dir.parent / "internal"
    yield package_dir.parents[2] / "bazel-bin" / "cyber" / "python" / "internal"


def _load_extension():
    module_name = f"{__package__}._cyber_wrapper" if __package__ else "_cyber_wrapper"
    existing = sys.modules.get(module_name)
    if existing is not None:
        return existing

    seen = set()
    for directory in _extension_directories():
        resolved = directory.resolve(strict=False)
        if resolved in seen or not directory.is_dir():
            continue
        seen.add(resolved)
        candidates = sorted(directory.glob("_cyber_wrapper*.so")) + sorted(
            directory.glob("_cyber_wrapper*.pyd")
        )
        for path in candidates:
            spec = importlib.util.spec_from_file_location(module_name, path)
            if spec is None or spec.loader is None:
                continue
            module = importlib.util.module_from_spec(spec)
            sys.modules[module_name] = module
            spec.loader.exec_module(module)
            return module

    searched = ", ".join(str(path) for path in _extension_directories())
    raise ImportError(f"Unable to locate _cyber_wrapper extension. Searched: {searched}")


_CYBER = _load_extension()

__all__ = ["_CYBER"]

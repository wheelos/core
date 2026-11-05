"""Load declared CcInfo/runtime-runfiles libraries after relocation."""

import json
import os
import shutil
import subprocess
import sys
import unittest
import uuid
from pathlib import Path

from runtime import runtime_entry


class RuntimeLibraryTest(unittest.TestCase):
    def test_python_executable_external_runfiles_are_relocatable(self):
        directory = Path(".runtime-python-" + uuid.uuid4().hex).resolve()
        directory.mkdir()
        self.addCleanup(shutil.rmtree, directory)
        root = directory / "relocated runtime"
        shutil.copytree(Path(__file__).parent / "fixture_layout", root)
        result = subprocess.run(
            [str(root / "bin/modules/example/fixture_diagnose"), "space argument"],
            env=dict(os.environ, LD_LIBRARY_PATH="/not/a/build/cache"),
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), [str(root), ["space argument"]])

    def test_declared_shared_library_closure_is_relocatable(self):
        directory = Path(".runtime-libraries-" + uuid.uuid4().hex).resolve()
        directory.mkdir()
        self.addCleanup(shutil.rmtree, directory)
        root = directory / "relocated runtime"
        shutil.copytree(Path(__file__).parent / "fixture_layout", root)
        self.assertTrue((root / "lib/libfixture_dependency.so").is_file())
        environment = runtime_entry.environment(
            root, dict(os.environ, LD_LIBRARY_PATH="/not/a/build/cache")
        )
        result = subprocess.run(
            [
                sys.executable,
                "-c",
                "import ctypes, sys; "
                "library = ctypes.CDLL(sys.argv[1]); "
                "sys.exit(0 if library.runtime_component_value() == 7 else 1)",
                str(root / "lib/components/example/libfixture_component.so"),
            ],
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()

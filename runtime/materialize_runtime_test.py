"""Filesystem and command contracts without starting Cyber or hardware."""

import hashlib
import json
import os
import shutil
import subprocess
import unittest
import uuid
from pathlib import Path

try:
    from runtime import materialize_runtime as materializer
    from runtime import runtime_entry as entry
except ImportError:
    import materialize_runtime as materializer
    import runtime_entry as entry


class RuntimeTest(unittest.TestCase):
    def setUp(self):
        self.directory = Path(".runtime-test-" + uuid.uuid4().hex).resolve()
        self.directory.mkdir()
        self.addCleanup(shutil.rmtree, self.directory)
        self.source = self.directory / "source"
        self.source.mkdir()
        self.runtime = self.directory / "runtime"

    def file(self, name, contents="test"):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        return str(path)

    def test_copy_directory_libraries_and_relocation(self):
        config = self.file("conf/example.pb.txt")
        library = self.file("libexample.so", "library")
        materializer.materialize(
            {
                "files": [
                    [config, "conf/example/example.pb.txt"],
                    [str(self.source / "conf"), "share/example/configs"],
                ],
                "libraries": [library],
                "default_launch": "launch/example/example.launch",
            },
            self.runtime,
        )
        moved = self.directory / "relocated runtime"
        self.runtime.rename(moved)
        shutil.rmtree(self.source)
        self.assertEqual((moved / "conf/example/example.pb.txt").read_text(), "test")
        self.assertEqual(
            (moved / "share/example/configs/example.pb.txt").read_text(), "test"
        )
        self.assertEqual((moved / "lib/libexample.so").read_text(), "library")
        self.assertFalse(any(path.is_symlink() for path in moved.rglob("*")))
        self.assertTrue((moved / "assets/models").is_dir())
        self.assertEqual(
            entry.command(moved, [])[1:],
            [
                str(moved / "bin/cyber_launch.py"),
                "start",
                str(moved / "launch/example/example.launch"),
            ],
        )

    def test_missing_traversal_conflicts_and_reserved_files(self):
        first = self.file("one", "one")
        second = self.file("two", "two")
        for manifest, exception in (
            ({"files": [["missing", "conf/example/file"]]}, FileNotFoundError),
            ({"files": [[first, "../escape"]]}, ValueError),
            ({"files": [[first, "conf/../escape"]]}, ValueError),
            ({"files": [[first, "/absolute"]]}, ValueError),
            ({"files": [[first, "conf//bad"]]}, ValueError),
            (
                {
                    "files": [
                        [first, "conf/example/file"],
                        [second, "conf/example/file"],
                    ]
                },
                ValueError,
            ),
            (
                {
                    "files": [
                        [first, "conf/example/file"],
                        [second, "conf/example/file/x"],
                    ]
                },
                ValueError,
            ),
            ({"files": [[first, "conf/runtime.json"]]}, ValueError),
            ({"default_dag": "a", "default_launch": "b"}, ValueError),
            ({"modules": ["example", "example"]}, ValueError),
            ({"required_configs": ["../outside"]}, ValueError),
            ({"required_assets": {"models/example/model": "invalid"}}, ValueError),
        ):
            with self.subTest(manifest=manifest), self.assertRaises(exception):
                materializer.materialize(manifest, self.runtime)
            self.assertFalse(self.runtime.exists())
        materializer.materialize(
            {"files": [[first, "share/example/file"], [first, "share/example/file"]]},
            self.runtime,
        )
        self.assertEqual((self.runtime / "share/example/file").read_text(), "one")

    def test_component_repository_mappings_merge_and_reject_conflicts(self):
        first = self.file(
            "first.repo_mapping",
            "camera+,core,wheelos_core+\nshared+,msgs,wheelos_msgs+\n",
        )
        second = self.file(
            "second.repo_mapping",
            "lidar+,core,wheelos_core+\nshared+,msgs,wheelos_msgs+\n",
        )
        destination = "share/hardware/runfiles/_repo_mapping"
        materializer.materialize(
            {"files": [[first, destination], [second, destination]]},
            self.runtime,
        )
        self.assertEqual(
            (self.runtime / destination).read_text().splitlines(),
            [
                "camera+,core,wheelos_core+",
                "lidar+,core,wheelos_core+",
                "shared+,msgs,wheelos_msgs+",
            ],
        )
        shutil.rmtree(self.runtime)
        conflicting = self.file(
            "conflicting.repo_mapping",
            "camera+,core,other_core+\n",
        )
        with self.assertRaisesRegex(ValueError, "conflicting repository mappings"):
            materializer.materialize(
                {"files": [[first, destination], [conflicting, destination]]},
                self.runtime,
            )
        self.assertFalse(self.runtime.exists())

    def test_package_partitions_preserve_executable_and_data_modes(self):
        executable = self.file("tool", "#!/bin/sh\nexit 0\n")
        Path(executable).chmod(0o755)
        resource = self.file("data", "data")
        Path(resource).chmod(0o555)
        modes = self.directory / "modes.json"
        materializer.materialize(
            {
                "files": [
                    [executable, "bin/modules/example/tool"],
                    [resource, "assets/example/data"],
                ]
            },
            self.runtime,
            modes,
        )
        for path in self.runtime.rglob("*"):
            if path.is_file():
                path.chmod(0o555)  # Bazel normalizes TreeArtifact children.
        binaries = self.directory / "package-executables"
        data = self.directory / "package-data"
        materializer.package_trees(self.runtime, binaries, data, modes)
        self.assertTrue((binaries / "bin/run").is_file())
        self.assertEqual((binaries / "bin/run").stat().st_mode & 0o777, 0o755)
        self.assertEqual((data / "assets/example/data").stat().st_mode & 0o777, 0o644)
        self.assertFalse((data / "bin/run").exists())
        self.assertFalse((binaries / "assets/example/data").exists())
        self.assertTrue((data / "assets/models").is_dir())

    def test_destination_symlink_escape_is_rejected(self):
        self.runtime.mkdir()
        (self.runtime / "conf").symlink_to(self.source, target_is_directory=True)
        with self.assertRaises(ValueError):
            materializer.materialize(
                {"files": [[self.file("one"), "conf/example/file"]]}, self.runtime
            )
        self.assertFalse((self.source / "example").exists())

    def test_runfiles_relative_symlink_resolves_only_declared_target(self):
        interpreter = self.file("python3", "interpreter")
        Path(interpreter).chmod(0o755)
        link = self.source / "venv_python"
        link.symlink_to("../../external/bin/python3")
        destination = "share/example/runfiles/main/venv/bin/python3"
        declared_target = "share/example/runfiles/main/external/bin/python3"
        materializer.materialize(
            {
                "files": [
                    [str(link), destination],
                    [interpreter, declared_target],
                ]
            },
            self.runtime,
        )
        self.assertEqual((self.runtime / destination).read_text(), "interpreter")
        self.assertFalse((self.runtime / destination).is_symlink())
        shutil.rmtree(self.runtime)
        with self.assertRaisesRegex(FileNotFoundError, "not declared"):
            materializer.materialize(
                {"files": [[str(link), destination]]}, self.runtime
            )
        self.assertFalse(self.runtime.exists())

    def test_module_executable_wrapper_preserves_declared_runfiles(self):
        executable = self.file(
            "diagnose.py",
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "data = Path(os.environ['RUNFILES_DIR']) / '_main/data.txt'\n"
            "print(json.dumps([data.read_text(), os.environ['CYBER_PATH'], "
            "os.environ['WHEELOS_ASSET_ROOT'], sys.argv[1:]]))\n"
            "sys.exit(19)\n",
        )
        Path(executable).chmod(0o755)
        destination = "bin/modules/example/diagnose"
        materializer.materialize(
            {
                "files": [
                    [executable, destination],
                    [
                        self.file("data.txt", "declared runfile"),
                        destination + ".runfiles/_main/data.txt",
                    ],
                ],
                "executables": [destination],
            },
            self.runtime,
        )
        moved = self.directory / "relocated runtime"
        self.runtime.rename(moved)
        result = subprocess.run(
            [str(moved / destination), "space argument"],
            env={"PATH": os.environ["PATH"], "LD_LIBRARY_PATH": "/caller/cache"},
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 19, result.stderr)
        self.assertEqual(
            json.loads(result.stdout),
            [
                "declared runfile",
                str(moved),
                str(moved / "assets"),
                ["space argument"],
            ],
        )

    def tools(self, requirements=None):
        fake = self.file(
            "fake.py",
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "print(json.dumps([sys.argv[1:], os.getcwd(), "
            "os.environ['APOLLO_ROOT_DIR'], os.environ['CYBER_PATH'], "
            "os.environ['LD_LIBRARY_PATH'], os.environ['WHEELOS_ASSET_ROOT'], "
            "os.getenv('WHEELOS_CONFIG_ROOT'), os.getenv('GLOG_logtostderr')]))\n"
            "sys.exit(17)\n",
        )
        manifest = {
            "files": [
                [fake, "bin/" + name]
                for name in (
                    "mainboard",
                    "cyber_launch.py",
                    "cyber_recorder",
                    "cyber_monitor",
                )
            ],
            **(requirements or {}),
        }
        materializer.materialize(manifest, self.runtime)
        return {
            "PATH": os.environ["PATH"],
            "LD_LIBRARY_PATH": "/caller/cache",
            "WHEELOS_RUNTIME_LIBRARY_PATH": "/reviewed/sdk",
        }

    def test_relocated_commands_preserve_arguments_cwd_and_exit(self):
        env = self.tools()
        moved = self.directory / "runtime with spaces"
        self.runtime.rename(moved)
        for name in materializer.COMMANDS:
            with self.subTest(command=name):
                result = subprocess.run(
                    [str(moved / "bin/commands" / name), "argument with spaces", "$x"],
                    cwd=self.directory,
                    env=env,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 17, result.stderr)
                payload = json.loads(result.stdout)
                self.assertEqual(payload[0], ["argument with spaces", "$x"])
                self.assertEqual(
                    payload[1],
                    str(
                        self.directory
                        if name in ("cyber_recorder", "cyber_monitor")
                        else moved
                    ),
                )
                self.assertEqual(
                    payload[2:],
                    [
                        str(moved),
                        str(moved),
                        str(moved / "lib") + ":/reviewed/sdk",
                        str(moved / "assets"),
                        None,
                        "1",
                    ],
                )
        result = subprocess.run(
            [
                "bash",
                "-c",
                'source "$1/bin/setup.bash"; mainboard "space argument"',
                "test",
                str(moved),
            ],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual(json.loads(result.stdout)[0], ["space argument"])

    def test_required_assets_and_explicit_configuration_block_start_only(self):
        model = self.file("model", "weights")
        digest = hashlib.sha256(b"weights").hexdigest()
        env = self.tools(
            {
                "required_assets": {"models/example/model": digest},
                "required_configs": ["conf/example/reviewed.pb.txt"],
            }
        )
        with self.assertRaisesRegex(ValueError, "WHEELOS_CONFIG_ROOT"):
            entry.validate_required(self.runtime, entry.environment(self.runtime, env))
        config = self.directory / "overlay"
        (config / "conf/example").mkdir(parents=True)
        (config / "conf/example/reviewed.pb.txt").write_text("reviewed")
        env["WHEELOS_CONFIG_ROOT"] = str(config)
        model_path = self.runtime / "assets/models/example/model"
        model_path.parent.mkdir(parents=True)
        shutil.copyfile(model, model_path)
        entry.validate_required(self.runtime, entry.environment(self.runtime, env))
        model_path.write_text("wrong")
        for name in ("mainboard", "cyber_launch", "cyber_recorder", "cyber_monitor"):
            result = subprocess.run(
                [str(self.runtime / "bin/commands" / name), "-h"],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                result.returncode,
                17 if name in ("cyber_recorder", "cyber_monitor") else 1,
                result.stderr,
            )
        model_path.unlink()
        model_path.symlink_to(model)
        with self.assertRaisesRegex(ValueError, "escapes root"):
            entry.validate_required(self.runtime, entry.environment(self.runtime, env))

    def test_entry_selection_and_overlay_keys(self):
        self.runtime.mkdir()
        self.assertEqual(
            entry.command(self.runtime, ["--dag", "override.dag", "--", "-p", "x"]),
            [
                str(self.runtime / "bin/mainboard"),
                "-d",
                str(self.runtime / "override.dag"),
                "-p",
                "x",
            ],
        )
        for arguments in (
            [],
            ["--dag", "a", "--launch", "b"],
            ["--launch", "a", "--", "-p", "x"],
        ):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit):
                entry.command(self.runtime, arguments)


if __name__ == "__main__":
    unittest.main()

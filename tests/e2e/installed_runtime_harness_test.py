#!/usr/bin/env python3
"""Stdlib regression tests; never imports pycyber or requires native artifacts."""

import hashlib
import json
import shutil
import signal
import subprocess
import unittest
import uuid
from pathlib import Path
from unittest import mock

import installed_runtime_test as harness

from artifacts import export_python_candidate, write_python_metadata


class HarnessTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(".installed-e2e-unit-" + uuid.uuid4().hex)
        self.root.mkdir()
        self.addCleanup(shutil.rmtree, self.root)

    def artifacts(self):
        (self.root / "core").mkdir()
        (self.root / "pycyber").mkdir()
        (self.root / "manifest.txt").write_text("candidate")
        deb = self.root / "core/core.deb"
        wheel = self.root / "pycyber/pycyber.whl"
        deb.write_bytes(b"native")
        wheel.write_bytes(b"wheel")
        (self.root / "SHA256SUMS").write_text(
            "".join(
                hashlib.sha256(path.read_bytes()).hexdigest()
                + "  "
                + str(path.relative_to(self.root))
                + "\n"
                for path in (deb, wheel)
            )
        )
        return deb, wheel

    def test_selects_only_unique_checksummed_candidates(self):
        deb, wheel = self.artifacts()
        self.assertEqual(
            (deb.resolve(), wheel.resolve()), harness.select_artifacts(self.root)
        )
        (self.root / "core/other.deb").write_bytes(b"other")
        with self.assertRaisesRegex(AssertionError, "exactly one"):
            harness.select_artifacts(self.root)

    def test_corrupted_or_unlisted_candidate_is_rejected(self):
        deb, _ = self.artifacts()
        deb.write_bytes(b"corrupted")
        with self.assertRaisesRegex(AssertionError, "checksum mismatch"):
            harness.select_artifacts(self.root)
        (self.root / "SHA256SUMS").write_text("")
        with self.assertRaisesRegex(AssertionError, "not checksummed"):
            harness.select_artifacts(self.root)

    def test_checksum_cannot_escape_artifact_directory(self):
        self.artifacts()
        (self.root / "SHA256SUMS").write_text("a" * 64 + "  ../outside\n")
        with self.assertRaisesRegex(AssertionError, "escapes artifact root"):
            harness.select_artifacts(self.root)

    def test_wheel_only_accepts_missing_native_but_full_mode_does_not(self):
        deb, wheel = self.artifacts()
        deb.unlink()
        (self.root / "SHA256SUMS").write_text(
            hashlib.sha256(wheel.read_bytes()).hexdigest() + "  pycyber/pycyber.whl\n"
        )
        self.assertEqual(
            (wheel.resolve(),), harness.select_artifacts(self.root, wheel_only=True)
        )
        with self.assertRaisesRegex(AssertionError, "exactly one core"):
            harness.select_artifacts(self.root)

    def test_ci_download_contains_only_self_contained_python_artifacts(self):
        _, wheel = self.artifacts()
        (self.root / "manifest.txt").write_text(
            "git_sha=commit\nbazel_version=bazel test\ncore_version=1.0.0\n"
        )
        sdist = self.root / "pycyber/pycyber-1.0.0.tar.gz"
        sdist.write_bytes(b"source")
        with (self.root / "SHA256SUMS").open("a") as checksums:
            checksums.write(
                hashlib.sha256(sdist.read_bytes()).hexdigest()
                + "  pycyber/pycyber-1.0.0.tar.gz\n"
            )
        exported = self.root / "exported"
        export_python_candidate(self.root, exported)
        downloaded = self.root / "downloaded"
        shutil.copytree(exported, downloaded)
        self.assertFalse((downloaded / "core").exists())
        self.assertEqual(
            ((downloaded / "pycyber" / wheel.name).resolve(),),
            harness.select_artifacts(downloaded, wheel_only=True),
        )
        self.assertEqual(
            sdist.read_bytes(), (downloaded / "pycyber" / sdist.name).read_bytes()
        )

    def test_wheel_stage_candidate_needs_no_native_or_sdist(self):
        (self.root / "pycyber").mkdir()
        wheel = self.root / "pycyber/pycyber.whl"
        wheel.write_bytes(b"wheel")
        write_python_metadata(
            self.root, {"git_sha": "commit", "bazel_version": "bazel test"}
        )
        downloaded = self.root / "downloaded"
        export_python_candidate(self.root, downloaded)
        self.assertEqual(
            ((downloaded / "pycyber" / wheel.name).resolve(),),
            harness.select_artifacts(downloaded, wheel_only=True),
        )

    def test_export_cannot_reauthorize_changed_producer_bytes(self):
        _, wheel = self.artifacts()
        wheel.write_bytes(b"changed after qualification")
        with self.assertRaisesRegex(ValueError, "changed before export"):
            export_python_candidate(self.root, self.root / "downloaded")
        self.assertFalse((self.root / "downloaded").exists())

    def test_optional_source_deliverable_must_be_checksummed(self):
        self.artifacts()
        sdist = self.root / "pycyber/pycyber-1.0.0.tar.gz"
        sdist.write_bytes(b"source")
        with self.assertRaisesRegex(AssertionError, "not checksummed"):
            harness.select_artifacts(self.root)

    def test_transport_configuration_uses_only_installed_resources(self):
        resources = self.root / "installed/cyber"
        (resources / "conf").mkdir(parents=True)
        config = resources / "conf/cyber.pb.conf"
        config.write_text(
            "transport_conf { communication_mode { diff_proc: ICEORYX } }\n"
        )
        candidate = object.__new__(harness.Harness)
        candidate.work = self.root
        candidate.resources = resources
        candidate.env = {"KEEP": "value"}
        env = candidate.mode_environment("SHM")
        self.assertEqual("value", env["KEEP"])
        copied = Path(env["CYBER_PATH"]) / "conf/cyber.pb.conf"
        self.assertIn("diff_proc: SHM", copied.read_text())
        self.assertIn("diff_proc: ICEORYX", config.read_text())
        config.write_text("transport_conf {}\n")
        with self.assertRaisesRegex(AssertionError, "explicit diff_proc"):
            candidate.mode_environment("RTPS")

    def test_native_mainboard_resolves_dag_within_installed_software_root(self):
        self.artifacts()
        fixtures = self.root / "fixtures"
        fixtures.mkdir()
        (fixtures / "libinstalled_e2e_component.so").touch()
        with mock.patch.dict(
            harness.os.environ,
            {
                "APOLLO_ROOT_DIR": "/apollo",
                "WHEELOS_CONFIG_ROOT": "/checkout/config",
                "WHEELOS_ASSET_ROOT": "/checkout/assets",
            },
        ):
            candidate = harness.Harness(self.root, fixtures, self.root / "results")
        for key in ("APOLLO_ROOT_DIR", "WHEELOS_CONFIG_ROOT", "WHEELOS_ASSET_ROOT"):
            self.assertNotIn(key, candidate.env)
        native = candidate.work / "native/opt/wheelos_core"
        (native / "bin").mkdir(parents=True)
        (native / "setup.bash").touch()
        for tool in ("mainboard", "cyber_recorder"):
            (native / "bin" / tool).touch()
        sourced_env = {
            **candidate.env,
            "CYBER_PATH": str(native / "resources/cyber"),
        }
        source_result = mock.Mock(
            stdout=b"\0".join(
                f"{key}={value}".encode() for key, value in sourced_env.items()
            )
            + b"\0"
        )
        with (
            mock.patch.object(candidate, "command"),
            mock.patch.object(harness.subprocess, "run", return_value=source_result),
        ):
            with self.assertRaisesRegex(AssertionError, "must export.*APOLLO_ROOT_DIR"):
                candidate.install()
            without_root = source_result.stdout
            source_result.stdout = without_root + b"APOLLO_ROOT_DIR=/apollo\0"
            with self.assertRaisesRegex(AssertionError, "select the extracted"):
                candidate.install()
            source_result.stdout = without_root + f"APOLLO_ROOT_DIR={native}\0".encode()
            candidate.install()
        self.assertEqual(str(native), candidate.processes.env["APOLLO_ROOT_DIR"])

        def start_mainboard(name, command, env):
            software_root = Path(env["APOLLO_ROOT_DIR"])
            self.assertEqual(native, software_root)
            self.assertTrue(software_root.is_absolute())
            self.assertTrue(software_root.is_dir())
            dag = Path(command[command.index("-d") + 1])
            self.assertTrue(dag.resolve().is_relative_to(software_root.resolve()))
            self.assertTrue(dag.is_file())
            Path(env["WHEELOS_E2E_COMPONENT_MARKER"]).write_text("1\n2\n3\n4\n5\n")
            return mock.sentinel.mainboard

        with (
            mock.patch.object(
                candidate.processes, "start", side_effect=start_mainboard
            ),
            mock.patch.object(candidate.processes, "stop") as stop,
            mock.patch.object(harness.time, "sleep"),
        ):
            self.assertEqual({"ticks": 5, "clean_sigint": True}, candidate.mainboard())
        stop.assert_called_once_with(mock.sentinel.mainboard)

    @mock.patch.object(harness.os, "killpg")
    def test_timeout_kills_and_reaps_child(self, killpg):
        process = mock.Mock(pid=123, returncode=0)
        process.poll.return_value = None
        process.wait.side_effect = [
            subprocess.TimeoutExpired("worker", 1),
            subprocess.TimeoutExpired("worker", 8),
            0,
        ]
        process.e2e_log = self.root / "worker.log"
        processes = harness.Processes(self.root, {})
        with self.assertRaisesRegex(AssertionError, "timed out"):
            processes.wait(process, timeout=1)
        self.assertEqual(
            [mock.call(123, signal.SIGINT), mock.call(123, signal.SIGKILL)],
            killpg.call_args_list,
        )
        self.assertEqual(3, process.wait.call_count)

    def test_nonzero_child_is_a_failure(self):
        process = mock.Mock(e2e_log="failure.log")
        process.wait.return_value = 23
        with self.assertRaisesRegex(AssertionError, "exit 23"):
            harness.Processes(self.root, {}).wait(process)

    def test_install_failure_writes_report_and_cleans_up(self):
        candidate = mock.Mock(outdir=self.root, work=self.root, wheel_only=False)
        candidate.install.side_effect = RuntimeError("bad candidate")
        self.assertEqual(1, harness.run_suite(candidate))
        report = json.loads((self.root / "report.json").read_text())
        self.assertEqual("failed", report["status"])
        self.assertEqual("bad candidate", report["scenarios"][0]["error"])
        candidate.processes.close.assert_called_once()
        candidate.pubsub.assert_not_called()

    def test_cli_writes_failed_full_scope_report_for_missing_candidate(self):
        outdir = self.root / "results"
        self.assertEqual(
            1,
            harness.main(
                [
                    "--artifacts",
                    str(self.root / "missing"),
                    "--fixtures",
                    str(self.root / "fixtures"),
                    "--outdir",
                    str(outdir),
                ]
            ),
        )
        report = json.loads((outdir / "report.json").read_text())
        self.assertEqual("failed", report["overall_status"])
        self.assertEqual("full", report["scope"])
        self.assertIn("manifest", report["scenarios"][0]["error"])

    def test_scenario_failure_is_not_hidden_by_later_success(self):
        candidate = mock.Mock(outdir=self.root, work=self.root, wheel_only=False)
        candidate.install.return_value = {}
        candidate.pubsub.side_effect = [RuntimeError("payload corruption"), {}]
        candidate.rpc.return_value = {}
        candidate.mainboard.return_value = {}
        self.assertEqual(1, harness.run_suite(candidate))
        report = json.loads((self.root / "report.json").read_text())
        self.assertEqual("failed", report["status"])
        self.assertEqual(5, len(report["scenarios"]))
        self.assertEqual(5, candidate.processes.close.call_count)

    def test_wheel_only_success_reports_scope_without_native_coverage(self):
        candidate = mock.Mock(outdir=self.root, work=self.root, wheel_only=True)
        candidate.install.return_value = {}
        candidate.pubsub.return_value = {}
        candidate.rpc.return_value = {}
        candidate.wheel_record.return_value = {}
        self.assertEqual(0, harness.run_suite(candidate))
        report = json.loads((self.root / "report.json").read_text())
        self.assertEqual("passed", report["overall_status"])
        self.assertEqual("wheel-only", report["scope"])
        self.assertEqual("wheel_record_write_read", report["scenarios"][-1]["name"])
        candidate.mainboard.assert_not_called()
        candidate.pubsub.assert_any_call("RTPS", record_native=False)

    def test_cleanup_error_fails_scenario_and_still_writes_report(self):
        candidate = mock.Mock(outdir=self.root, work=self.root, wheel_only=False)
        candidate.install.return_value = {}
        candidate.processes.close.side_effect = RuntimeError("unreaped child")
        self.assertEqual(1, harness.run_suite(candidate))
        report = json.loads((self.root / "report.json").read_text())
        self.assertEqual("failed", report["overall_status"])
        self.assertIn("cleanup failed", report["scenarios"][0]["error"])


if __name__ == "__main__":
    unittest.main()

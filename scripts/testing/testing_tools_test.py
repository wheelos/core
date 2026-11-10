#!/usr/bin/env python3

import json
import subprocess
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

import check_test_layers
import container_test
import qualify_release


class SnapshotTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.repo = self.directory / "repo"
        self.repo.mkdir()
        self.cache = self.directory / "cache"
        self.git("init", "--quiet")
        self.git("config", "user.name", "Snapshot Test")
        self.git("config", "user.email", "test@example.invalid")
        (self.repo / "source.txt").write_text("initial\n")
        self.git("add", "source.txt")
        self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "initial")

    def git(self, *args, root=None):
        return subprocess.run(
            ["git", "-C", str(root or self.repo), *args],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def snapshot(self):
        return container_test.prepare_workspace(self.cache, self.repo)

    def test_committing_tested_contents_refreshes_head_and_clean_state(self):
        (self.repo / "source.txt").write_text("updated\n")
        self.git("add", "source.txt")
        before = self.snapshot()
        fingerprint = container_test.source_fingerprint(self.repo)
        self.assertIn("updated", self.git("diff", "--cached", root=before))
        self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "updated")
        after = self.snapshot()
        self.assertEqual(fingerprint, container_test.source_fingerprint(self.repo))
        self.assertNotEqual(before, after)
        self.assertEqual(
            self.git("rev-parse", "HEAD"), self.git("rev-parse", "HEAD", root=after)
        )
        self.assertEqual("", self.git("status", "--porcelain", root=after))

    def test_new_tag_refreshes_identity_without_recopying_unchanged_state(self):
        before = self.snapshot()
        self.git("status", "--porcelain", root=before)
        self.assertEqual(before, self.snapshot())
        self.git("tag", "wheelos_core-v1.0.0")
        after = self.snapshot()
        self.assertNotEqual(before, after)
        self.assertEqual(
            "wheelos_core-v1.0.0", self.git("tag", "--points-at", "HEAD", root=after)
        )

    def test_worktree_snapshot_keeps_staged_objects_without_original_repo(self):
        linked = self.directory / "linked"
        self.git("worktree", "add", "--quiet", "-b", "feature", str(linked))
        (linked / "source.txt").write_text("staged\n")
        self.git("add", "source.txt", root=linked)
        (linked / "source.txt").write_text("unstaged\n")
        snapshot = container_test.prepare_workspace(self.cache, linked)
        expected = self.git("status", "--porcelain", root=linked)
        self.repo.rename(self.directory / "unavailable")
        self.assertEqual(expected, self.git("status", "--porcelain", root=snapshot))
        self.assertIn("staged", self.git("diff", "--cached", root=snapshot))
        self.assertEqual("initial", self.git("show", "HEAD:source.txt", root=snapshot))
        self.git("fetch", "--tags", "--prune", "origin", root=snapshot)


class ContainerTest(unittest.TestCase):
    def test_configured_long_duration_cannot_replace_measured_elapsed_time(self):
        with tempfile.TemporaryDirectory() as directory:
            args = container_test.parse_args(
                ["perf", "--outdir", directory, "--cache-dir", directory]
            )
            result = Path(directory) / "perf"
            (result / "acceptance").mkdir(parents=True)
            (result / "acceptance/report.json").write_text(
                json.dumps({"overall_status": "passed"})
            )
            (result / "long-run.json").write_text(
                json.dumps(
                    {
                        "results": [
                            {
                                "scenario": "long_run",
                                "success": True,
                                "duration_s": 7200,
                                "reliability": {"total_loss": 0},
                            }
                        ]
                    }
                )
            )
            (result / "long-run-timing.json").write_text(
                json.dumps({"elapsed_seconds": 5})
            )
            with mock.patch.object(
                container_test,
                "run",
                return_value=subprocess.CompletedProcess([], 0, stdout="identity"),
            ), mock.patch.object(
                container_test, "source_fingerprint", return_value="source"
            ), mock.patch.object(
                container_test.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, stdout="[]"),
            ):
                with self.assertRaisesRegex(RuntimeError, "elapsed time"):
                    container_test.execute(args, "perf")

    def test_source_mutation_cannot_promote_a_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            args = container_test.parse_args(
                ["package", "--outdir", directory, "--cache-dir", directory]
            )
            with mock.patch.object(
                container_test,
                "run",
                return_value=subprocess.CompletedProcess([], 0, stdout="identity"),
            ), mock.patch.object(
                container_test, "source_fingerprint", side_effect=["before", "after"]
            ), mock.patch.object(
                container_test.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, stdout="[]"),
            ), mock.patch.object(
                container_test, "install_candidate"
            ) as install:
                with self.assertRaisesRegex(RuntimeError, "Source files changed"):
                    container_test.execute(args, "package")
                install.assert_not_called()

    def test_build_containers_sharing_output_cache_are_serialized(self):
        first_entered = threading.Event()
        second_requested = threading.Event()
        second_entered = threading.Event()
        release_first = threading.Event()

        def container(args, stage):
            if stage == "unit":
                first_entered.set()
                if not release_first.wait(5):
                    raise TimeoutError("First container was not released")
            else:
                second_entered.set()

        def second(args):
            second_requested.set()
            container_test.execute(args, "integration")

        with tempfile.TemporaryDirectory() as directory:
            args = container_test.parse_args(["unit", "--cache-dir", directory])
            with mock.patch.object(
                container_test, "execute_container", side_effect=container
            ), ThreadPoolExecutor(max_workers=2) as pool:
                first = pool.submit(container_test.execute, args, "unit")
                try:
                    self.assertTrue(first_entered.wait(5))
                    next_container = pool.submit(second, args)
                    self.assertTrue(second_requested.wait(5))
                    self.assertFalse(second_entered.wait(0.1))
                finally:
                    release_first.set()
                first.result(timeout=5)
                next_container.result(timeout=5)
                self.assertTrue(second_entered.is_set())

    def test_candidate_replacement_preserves_previously_qualified_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = root / "candidate"
            new = root / "staging"
            old.mkdir()
            new.mkdir()
            (old / "old.whl").write_text("qualified")
            (new / "new.whl").write_text("new")
            container_test.install_candidate(new, old)
            self.assertEqual(["new.whl"], [path.name for path in old.iterdir()])
            archived = list(root.glob("candidate.previous-*"))
            self.assertEqual("qualified", (archived[0] / "old.whl").read_text())

    def test_empty_or_flaky_bazel_report_cannot_pass_a_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            events = Path(directory) / "events.json"
            for content in (
                "",
                json.dumps(
                    {
                        "id": {"testSummary": {"label": "//tests:case"}},
                        "testSummary": {"overallStatus": "FLAKY"},
                    }
                ),
            ):
                events.write_text(content)
                with self.assertRaises(RuntimeError):
                    container_test.bazel_results(events)

    def test_e2e_does_not_mount_checkout_or_build_cache(self):
        args = container_test.parse_args(["e2e"])
        command = container_test.docker_command(args, "e2e", "case", "runtime")
        text = " ".join(command)
        self.assertIn("--ipc=private", command)
        self.assertIn("--shm-size=1g", command)
        self.assertIn("dst=/artifacts,readonly", text)
        self.assertIn("dst=/cases,readonly", text)
        self.assertNotIn("dst=/workspace", text)
        self.assertNotIn("dst=/cache", text)
        self.assertNotIn("--privileged", command)
        self.assertNotIn("--network=host", command)

    def test_invoking_numeric_identity_and_explicit_cache_are_used(self):
        args = container_test.parse_args(
            ["integration", "--repository-cache", "/tmp/existing-repository"]
        )
        with mock.patch("os.getuid", return_value=1234), mock.patch(
            "os.getgid", return_value=5678
        ):
            command = container_test.docker_command(
                args, "integration", "case", "build"
            )
        self.assertIn("1234:5678", command)
        self.assertIn(
            "type=bind,src=/tmp/existing-repository,dst=/cache/repository",
            command,
        )

    def test_container_failure_is_reported_and_propagated(self):
        with tempfile.TemporaryDirectory() as directory:
            previous = Path(directory) / "integration"
            previous.mkdir()
            (previous / "container-report.json").write_text('{"previous": true}')
            args = container_test.parse_args(
                [
                    "integration",
                    "--outdir",
                    directory,
                    "--cache-dir",
                    str(Path(directory) / "cache"),
                ]
            )
            with mock.patch.object(
                container_test,
                "run",
                return_value=subprocess.CompletedProcess([], 0, stdout="identity\n"),
            ), mock.patch.object(
                container_test,
                "source_fingerprint",
                return_value="source",
            ), mock.patch.object(
                container_test.subprocess,
                "run",
                side_effect=[
                    subprocess.CompletedProcess([], 9),
                    subprocess.CompletedProcess([], 0),
                    subprocess.CompletedProcess([], 0, stdout="[]"),
                    subprocess.CompletedProcess([], 0),
                    subprocess.CompletedProcess([], 0),
                ],
            ):
                with self.assertRaises(RuntimeError):
                    container_test.execute(args, "integration")
            report = json.loads(
                (Path(directory) / "integration/container-report.json").read_text()
            )
            self.assertEqual("failed", report["overall_status"])
            self.assertEqual(9, report["exit_code"])
            archived = list(Path(directory).glob("integration.previous-*"))
            self.assertTrue(
                json.loads((archived[0] / "container-report.json").read_text())[
                    "previous"
                ]
            )


class LayerInventoryTest(unittest.TestCase):
    def test_new_unclassified_test_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tests").mkdir()
            (root / "tests/BUILD").write_text(
                'cc_test(name="orphan")\n'
                'test_suite(name="unit", tests=[])\n'
                'test_suite(name="integration", tests=[])\n'
                'test_suite(name="performance_regression", tests=[])\n'
                'test_suite(name="hardware", tests=[])\n'
            )
            with self.assertRaisesRegex(ValueError, "orphan"):
                check_test_layers.check(root)

    def test_manual_fixture_is_not_silently_put_into_cpu_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tests").mkdir()
            (root / "tests/BUILD").write_text(
                'cc_test(name="fixture", tags=["manual"])\n'
                'test_suite(name="unit", tests=[])\n'
                'test_suite(name="integration", tests=[])\n'
                'test_suite(name="performance_regression", tests=[])\n'
                'test_suite(name="hardware", tests=[])\n'
            )
            check_test_layers.check(root)


class QualificationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.candidate = Path(self.temp.name)
        for directory in ("core", "pycyber"):
            (self.candidate / directory).mkdir()
        for name in (
            "core/core.deb",
            "pycyber/package.whl",
            "pycyber/package.tar.gz",
            "manifest.txt",
            "SHA256SUMS",
        ):
            (self.candidate / name).write_text(name)
        (self.candidate / "manifest.txt").write_text(
            "git_sha=commit\ncore_version=1.0.0\nbazel_version=bazel test\n"
        )
        self.report = {
            "schema_version": 1,
            "overall_status": "passed",
            "git_sha": "commit",
            "source_dirty": False,
            "long_run_seconds": 7200,
            "gates": {
                name: {
                    "overall_status": "passed",
                    "git_sha": "commit",
                    "image_id": "sha256:image",
                }
                for name in qualify_release.GATES
            },
            "artifacts": qualify_release.artifact_hashes(self.candidate),
            "source_fingerprint": "source",
        }
        for gate in self.report["gates"].values():
            gate["source_fingerprint"] = "source"
        for name in ("unit", "integration"):
            self.report["gates"][name]["tests"] = {"//tests:case": "PASSED"}
        self.report["gates"]["e2e"].update(
            scope="full", tested_artifacts=self.report["artifacts"]
        )
        self.report["gates"]["perf"]["long_run_seconds"] = 7200
        self.report["gates"]["perf"]["long_run_elapsed_seconds"] = 7200

    def test_exact_qualified_artifacts_are_accepted(self):
        qualify_release.verify(self.report, self.candidate, "commit", publish=True)

    def test_missing_bazel_evidence_or_mixed_build_images_are_rejected(self):
        self.report["gates"]["unit"]["tests"] = {}
        with self.assertRaises(ValueError):
            qualify_release.verify(self.report, self.candidate, "commit")
        self.report["gates"]["unit"]["tests"] = {"//tests:case": "PASSED"}
        self.report["gates"]["package"]["image_id"] = "sha256:other-image"
        with self.assertRaises(ValueError):
            qualify_release.verify(self.report, self.candidate, "commit")

    def test_incomplete_candidate_manifest_is_rejected(self):
        (self.candidate / "manifest.txt").write_text("git_sha=commit\n")
        with self.assertRaisesRegex(ValueError, "manifest"):
            qualify_release.verify(self.report, self.candidate, "commit")

    def test_rebuilt_or_added_artifacts_are_rejected(self):
        (self.candidate / "pycyber/package.whl").write_text("rebuilt")
        with self.assertRaisesRegex(ValueError, "changed"):
            qualify_release.verify(self.report, self.candidate, "commit")

    def test_missing_gate_short_stability_or_dirty_publish_is_rejected(self):
        for field, value in (
            ("gates", {}),
            ("long_run_seconds", 5),
            ("source_dirty", True),
            ("git_sha", "other"),
        ):
            with self.subTest(field=field):
                report = dict(self.report, **{field: value})
                with self.assertRaises(ValueError):
                    qualify_release.verify(
                        report, self.candidate, "commit", publish=True
                    )

    def test_wheel_only_e2e_cannot_authorize_native_release(self):
        self.report["gates"]["e2e"]["scope"] = "wheel"
        with self.assertRaisesRegex(ValueError, "full package"):
            qualify_release.verify(self.report, self.candidate, "commit")

    def test_architecture_wheel_must_have_exact_installation_evidence(self):
        evidence = self.candidate / "reports"
        evidence.mkdir()
        wheel = self.candidate / "pycyber/package.whl"
        report = {
            "stage": "e2e",
            "overall_status": "passed",
            "git_sha": "commit",
            "image_id": "sha256:image",
            "tested_artifacts": {
                "pycyber/package.whl": container_test.digest_file(wheel)
            },
        }
        (evidence / "container-report.json").write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError, "no exact"):
            qualify_release.verify_distributions(
                self.candidate / "pycyber", evidence, "commit"
            )
        report["tested_artifacts"]["pycyber/package.tar.gz"] = (
            container_test.digest_file(self.candidate / "pycyber/package.tar.gz")
        )
        (evidence / "container-report.json").write_text(json.dumps(report))
        qualify_release.verify_distributions(
            self.candidate / "pycyber", evidence, "commit"
        )


if __name__ == "__main__":
    unittest.main()

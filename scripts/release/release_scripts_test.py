#!/usr/bin/env python3

import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import unittest


class ReleaseScriptsTest(unittest.TestCase):
    def setUp(self):
        self.work = Path.cwd() / ".release_scripts_test_work"
        shutil.rmtree(self.work, ignore_errors=True)
        self.repo = self.work / "repo"
        self.fake_bin = self.work / "bin"
        self.repo.mkdir(parents=True)
        self.fake_bin.mkdir(parents=True)
        self.call_log = self.work / "calls.log"
        self._write_executable(
            "git",
            """#!/bin/bash
if [ "$1 $2" = "rev-parse --show-toplevel" ]; then
  printf '%s\n' "$PWD"
elif [ "$1 $2" = "rev-parse HEAD" ]; then
  echo deadbeef
else
  exit 1
fi
""",
        )

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)

    def _script(self, name):
        return Path(__file__).resolve().with_name(name)

    def _write_executable(self, name, contents):
        path = self.fake_bin / name
        path.write_text(contents, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def _env(self, **updates):
        env = dict(os.environ)
        env.update(
            {
                "PATH": str(self.fake_bin) + os.pathsep + env["PATH"],
                "CALL_LOG": str(self.call_log),
            }
        )
        env.pop("CYBER_RECORD_PLAY_FIXTURE", None)
        env.update(updates)
        return env

    def _install_fake_bazel(self, cquery_output=""):
        self._write_executable(
            "bazel",
            f"""#!/bin/bash
echo "bazel $*" >> "$CALL_LOG"
case "$1" in
  --version) echo "bazel fake";;
  cquery) printf '%b' {cquery_output!r};;
esac
""",
        )

    def _install_acceptance_bash(self, build_exit):
        self._write_executable(
            "bash",
            f"""#!/bin/bash
echo "bash $*" >> "$CALL_LOG"
case "$1" in
  scripts/release/check_bzlmod_lockfile.sh) exit 0;;
  scripts/release/build_release_artifacts.sh) exit {build_exit};;
  *) exit 0;;
esac
""",
        )

    def _install_fake_dpkg(self):
        self._write_executable(
            "dpkg-deb",
            """#!/bin/bash
dest="$3"
root="$dest/opt/wheelos_core"
mkdir -p "$root/bin"
cat > "$root/setup.bash" <<'EOF'
export PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/bin:$PATH"
EOF
for tool in mainboard cyber_recorder cyber_monitor cyber_launch; do
  cat > "$root/bin/$tool" <<'EOF'
#!/bin/bash
exit 0
EOF
  chmod +x "$root/bin/$tool"
done
""",
        )

    def test_acceptance_only_skips_baseline_when_requested(self):
        self._install_fake_bazel()
        self._install_acceptance_bash(build_exit=7)

        for skip_requested in (False, True):
            with self.subTest(skip_requested=skip_requested):
                self.call_log.write_text("", encoding="utf-8")
                outdir = self.repo / ("skip" if skip_requested else "default")
                command = [
                    "/bin/bash",
                    str(self._script("run_release_acceptance.sh")),
                    "--stage",
                    "packaging",
                    "--outdir",
                    str(outdir),
                ]
                if skip_requested:
                    command.append("--skip-baseline")
                result = subprocess.run(
                    command,
                    cwd=self.repo,
                    env=self._env(),
                    capture_output=True,
                    text=True,
                )
                self.assertNotEqual(0, result.returncode)
                build_call = next(
                    line
                    for line in self.call_log.read_text(encoding="utf-8").splitlines()
                    if "build_release_artifacts.sh" in line
                )
                self.assertEqual(
                    skip_requested,
                    "--skip-baseline" in build_call,
                    build_call,
                )
                report = json.loads((outdir / "report.json").read_text())
                self.assertEqual("failed", report["overall_status"])
                self.assertTrue(report["stage_results"])

    def test_runtime_report_records_self_contained_fixture_coverage(self):
        self._install_fake_bazel()
        outdir = self.repo / "runtime-success"
        result = subprocess.run(
            [
                "/bin/bash",
                str(self._script("run_release_acceptance.sh")),
                "--stage",
                "runtime",
                "--outdir",
                str(outdir),
            ],
            cwd=self.repo,
            env=self._env(),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        report = json.loads((outdir / "report.json").read_text())
        self.assertEqual("passed", report["overall_status"])
        self.assertEqual(
            {
                "mode": "self_contained",
                "path": None,
                "self_contained": "actual",
                "external": "skipped",
            },
            report["fixture_coverage"],
        )
        calls = self.call_log.read_text(encoding="utf-8")
        self.assertEqual(1, calls.count("core_tool_matrix_tests"))
        self.assertNotIn("examples_regression_tests", calls)

    def test_explicit_missing_fixture_fails_acceptance(self):
        self._install_fake_bazel()
        missing = self.repo / "missing.record"
        outdir = self.repo / "missing-fixture"
        result = subprocess.run(
            [
                "/bin/bash",
                str(self._script("run_release_acceptance.sh")),
                "--stage",
                "runtime",
                "--outdir",
                str(outdir),
            ],
            cwd=self.repo,
            env=self._env(CYBER_RECORD_PLAY_FIXTURE=str(missing)),
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        report = json.loads((outdir / "report.json").read_text())
        self.assertEqual("failed", report["overall_status"])
        self.assertEqual("failed", report["fixture_coverage"]["external"])
        self.assertNotIn(
            "core_tool_matrix_tests",
            self.call_log.read_text(encoding="utf-8"),
        )

    def test_explicit_fixture_is_forwarded_and_reported_as_actual(self):
        self._install_fake_bazel()
        fixture = self.repo / "fixture.record"
        fixture.write_text("record", encoding="utf-8")
        outdir = self.repo / "external-fixture"
        result = subprocess.run(
            [
                "/bin/bash",
                str(self._script("run_release_acceptance.sh")),
                "--stage",
                "runtime",
                "--outdir",
                str(outdir),
            ],
            cwd=self.repo,
            env=self._env(CYBER_RECORD_PLAY_FIXTURE=str(fixture)),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        report = json.loads((outdir / "report.json").read_text())
        self.assertEqual("actual", report["fixture_coverage"]["external"])
        self.assertEqual("skipped", report["fixture_coverage"]["self_contained"])
        self.assertIn(
            f"--test_env=CYBER_RECORD_PLAY_FIXTURE={fixture}",
            self.call_log.read_text(encoding="utf-8"),
        )

    def test_artifact_builder_validates_and_copies_ci_deb(self):
        deb = self.repo / "bazel-bin" / "wheelos_core_1.0.4_amd64.deb"
        deb.parent.mkdir(parents=True)
        deb.write_text("ci package", encoding="utf-8")
        self._install_fake_bazel("bazel-bin/wheelos_core_1.0.4_amd64.deb\n")
        self._write_executable(
            "bash",
            """#!/bin/bash
echo "bash $*" >> "$CALL_LOG"
exit 0
""",
        )
        outdir = self.repo / "release"
        result = subprocess.run(
            [
                "/bin/bash",
                str(self._script("build_release_artifacts.sh")),
                "--outdir",
                str(outdir),
                "--skip-baseline",
                "--skip-pycyber",
            ],
            cwd=self.repo,
            env=self._env(),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        calls = self.call_log.read_text(encoding="utf-8")
        self.assertIn("bazel build --config=ci", calls)
        self.assertIn("bazel cquery --config=ci", calls)
        self.assertIn(f"--deb {deb}", calls)
        self.assertEqual(
            "ci package",
            (outdir / "core" / deb.name).read_text(encoding="utf-8"),
        )

    def test_baseline_uses_single_core_matrix_and_forwards_fixture(self):
        self._install_fake_bazel()
        self._write_executable(
            "bash",
            """#!/bin/bash
echo "bash $*" >> "$CALL_LOG"
exit 0
""",
        )
        fixture = self.repo / "fixture.record"
        fixture.write_text("record", encoding="utf-8")
        result = subprocess.run(
            ["/bin/bash", str(self._script("ubuntu2204_baseline.sh"))],
            cwd=self.repo,
            env=self._env(CYBER_RECORD_PLAY_FIXTURE=str(fixture)),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        calls = self.call_log.read_text(encoding="utf-8")
        self.assertEqual(1, calls.count("core_tool_matrix_tests"))
        self.assertNotIn("examples_regression_tests", calls)
        self.assertIn(
            f"--test_env=CYBER_RECORD_PLAY_FIXTURE={fixture}",
            calls,
        )

    def test_downstream_sdk_declares_direct_build_rule_dependencies(self):
        self._write_executable(
            "bazel",
            """#!/bin/bash
grep -q 'bazel_dep(name = "rules_cc", version = "0.0.9")' MODULE.bazel
grep -q 'bazel_dep(name = "rules_python", version = "0.34.0")' MODULE.bazel
grep -q 'python_version = "3.10"' MODULE.bazel
echo "bazel $*" >> "$CALL_LOG"
""",
        )
        result = subprocess.run(
            [
                "/bin/bash",
                str(self._script("validate_downstream_bazel_sdk.sh")),
            ],
            cwd=self.repo,
            env=self._env(),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        calls = self.call_log.read_text(encoding="utf-8")
        self.assertIn("//:cpp_consumer", calls)
        self.assertIn("//:python_consumer", calls)

    def test_exact_deb_validation_does_not_invoke_bazel(self):
        deb = self.repo / "input.deb"
        deb.write_text("package", encoding="utf-8")
        self._write_executable(
            "bazel",
            """#!/bin/bash
echo "unexpected bazel $*" >> "$CALL_LOG"
exit 99
""",
        )
        self._install_fake_dpkg()
        result = subprocess.run(
            [
                "/bin/bash",
                str(self._script("validate_runtime_bundle.sh")),
                "--deb",
                str(deb),
                "--workdir",
                str(self.repo / "validation"),
            ],
            cwd=self.repo,
            env=self._env(),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn(
            "unexpected bazel",
            self.call_log.read_text(encoding="utf-8")
            if self.call_log.exists()
            else "",
        )

    def test_config_validation_builds_and_queries_same_configuration(self):
        deb = self.repo / "bazel-out" / "k8-opt" / "bin" / "wheelos_core.deb"
        deb.parent.mkdir(parents=True)
        deb.write_text("package", encoding="utf-8")
        self._install_fake_bazel("bazel-out/k8-opt/bin/wheelos_core.deb\n")
        self._install_fake_dpkg()
        result = subprocess.run(
            [
                "/bin/bash",
                str(self._script("validate_runtime_bundle.sh")),
                "--config",
                "ci",
                "--workdir",
                str(self.repo / "validation"),
            ],
            cwd=self.repo,
            env=self._env(),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        calls = self.call_log.read_text(encoding="utf-8")
        self.assertEqual(1, calls.count("bazel build"))
        self.assertEqual(1, calls.count("bazel cquery"))
        self.assertIn("bazel build --config=ci", calls)
        self.assertIn("bazel cquery --config=ci", calls)


if __name__ == "__main__":
    unittest.main()

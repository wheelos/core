#!/usr/bin/env python3

import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import unittest
import zipfile


class ReleaseScriptsTest(unittest.TestCase):
    def setUp(self):
        self.work = Path.cwd() / ".release_scripts_test_work"
        shutil.rmtree(self.work, ignore_errors=True)
        self.repo = self.work / "repo"
        self.fake_bin = self.work / "bin"
        self.repo.mkdir(parents=True)
        self.fake_bin.mkdir(parents=True)
        self.call_log = self.work / "calls.log"
        (self.repo / "MODULE.bazel").write_text(
            'module(\n    name = "wheelos_core",\n    version = "1.0.5",\n)\n',
            encoding="utf-8",
        )
        (self.repo / "BUILD").write_text(
            'pkg_deb(\n    name = "wheelos_core",\n    version = "1.0.5",\n)\n',
            encoding="utf-8",
        )
        checker = self.repo / "scripts" / "release" / "check_release_version.py"
        checker.parent.mkdir(parents=True)
        shutil.copyfile(
            Path(__file__).resolve().with_name("check_release_version.py"), checker
        )
        self._write_executable(
            "git",
            """#!/bin/bash
if [ "$1 $2" = "rev-parse --show-toplevel" ]; then
  printf '%s\n' "$PWD"
elif [ "$1 $2" = "rev-parse HEAD" ]; then
  echo deadbeef
elif [ "$1 $2" = "rev-parse --short" ]; then
  echo deadbee
elif [ "$1 $2" = "rev-list --count" ]; then
  echo 144
elif [ "$1 $2" = "tag --points-at" ]; then
  if [ -n "${MOCK_RELEASE_TAG:-}" ]; then
    printf '%s\n' "$MOCK_RELEASE_TAG"
  fi
elif [ "$1 $2" = "status --porcelain" ]; then
  printf '%s' "${MOCK_GIT_DIRTY:-}"
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
        for name in ("CYBER_RECORD_PLAY_FIXTURE", "PYCYBER_VERSION",
                     "GITHUB_REF", "MOCK_RELEASE_TAG", "MOCK_GIT_DIRTY"):
            env.pop(name, None)
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
if [ "$1" = "-f" ]; then
  case "$3" in
    Package) echo "${MOCK_DEB_PACKAGE:-wheelos_core}";;
    Version) echo "${MOCK_DEB_VERSION:-1.0.5}";;
    *) exit 1;;
  esac
  exit 0
fi
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
        deb = self.repo / "bazel-bin" / "wheelos_core_1.0.5_amd64.deb"
        deb.parent.mkdir(parents=True)
        deb.write_text("ci package", encoding="utf-8")
        self._install_fake_bazel("bazel-bin/wheelos_core_1.0.5_amd64.deb\n")
        self._install_fake_dpkg()
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
        self.assertIn(
            "core_version=1.0.5",
            (outdir / "manifest.txt").read_text(encoding="utf-8"),
        )

    def test_release_version_rejects_drift_and_wrong_tag(self):
        script = str(self._script("check_release_version.py"))

        def verify(**env):
            return subprocess.run(
                [sys.executable, script, "--release"],
                cwd=self.repo,
                env=self._env(**env),
                capture_output=True,
                text=True,
            )

        self.assertEqual("1.0.5", verify().stdout.strip())
        self.assertEqual(
            "1.0.5", verify(MOCK_RELEASE_TAG="wheelos_core-v1.0.5").stdout.strip()
        )
        wrong_tag = verify(MOCK_RELEASE_TAG="wheelos_core-v1.0.4")
        self.assertNotEqual(0, wrong_tag.returncode)
        self.assertIn("release tag(s)", wrong_tag.stderr)
        wrong_wheel = verify(
            MOCK_RELEASE_TAG="wheelos_core-v1.0.5", PYCYBER_VERSION="1.0.4"
        )
        self.assertNotEqual(0, wrong_wheel.returncode)
        self.assertIn("PYCYBER_VERSION", wrong_wheel.stderr)
        (self.repo / "BUILD").write_text(
            'pkg_deb(\n    name = "wheelos_core",\n    version = "1.0.4",\n)\n',
            encoding="utf-8",
        )
        drift = verify()
        self.assertNotEqual(0, drift.returncode)
        self.assertIn("Debian package version", drift.stderr)

    def test_artifact_builder_rejects_wrong_deb_before_copy(self):
        deb = self.repo / "bazel-bin" / "wheelos_core_1.0.4_amd64.deb"
        deb.parent.mkdir(parents=True)
        deb.write_text("old package", encoding="utf-8")
        self._install_fake_bazel("bazel-bin/wheelos_core_1.0.4_amd64.deb\n")
        self._install_fake_dpkg()
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
        self.assertNotEqual(0, result.returncode)
        self.assertIn("unexpected Debian artifact", result.stderr)
        self.assertEqual([], list((outdir / "core").iterdir()))

    def test_artifact_builder_rejects_renamed_old_deb(self):
        deb = self.repo / "bazel-bin" / "wheelos_core_1.0.5_amd64.deb"
        deb.parent.mkdir(parents=True)
        deb.write_text("old package renamed", encoding="utf-8")
        self._install_fake_bazel("bazel-bin/wheelos_core_1.0.5_amd64.deb\n")
        self._install_fake_dpkg()
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
            env=self._env(MOCK_DEB_VERSION="1.0.4"),
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("Debian artifact metadata", result.stderr)
        self.assertEqual([], list((outdir / "core").iterdir()))

    def test_tagged_release_rejects_wrong_wheel_version(self):
        wheelhouse = self.repo / "wheelhouse"
        wheelhouse.mkdir()
        wheel = wheelhouse / "pycyber-1.0.4-py3-none-any.whl"
        with zipfile.ZipFile(wheel, "w") as archive:
            archive.writestr(
                "pycyber-1.0.4.dist-info/METADATA",
                "Name: pycyber\nVersion: 1.0.4\n",
            )
        command = [
            sys.executable,
            str(self._script("check_release_version.py")),
            "--release",
            "--wheelhouse",
            str(wheelhouse),
        ]
        result = subprocess.run(
            command,
            cwd=self.repo,
            env=self._env(MOCK_RELEASE_TAG="wheelos_core-v1.0.5"),
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("unexpected pycyber artifact", result.stderr)
        wheel.rename(wheelhouse / "pycyber-1.0.5-py3-none-any.whl")
        result = subprocess.run(
            command,
            cwd=self.repo,
            env=self._env(MOCK_RELEASE_TAG="wheelos_core-v1.0.5"),
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("pycyber artifact metadata", result.stderr)
        wheel = wheelhouse / "pycyber-1.0.5-py3-none-any.whl"
        with zipfile.ZipFile(wheel, "w") as archive:
            archive.writestr(
                "pycyber-1.0.5.dist-info/METADATA",
                "Name: pycyber\nVersion: 1.0.5\n",
            )
        result = subprocess.run(
            command,
            cwd=self.repo,
            env=self._env(MOCK_RELEASE_TAG="wheelos_core-v1.0.5"),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)

    def test_tagged_release_rejects_sdist_metadata_drift(self):
        wheelhouse = self.repo / "wheelhouse"
        wheelhouse.mkdir()
        wheel = wheelhouse / "pycyber-1.0.5-py3-none-any.whl"
        with zipfile.ZipFile(wheel, "w") as archive:
            archive.writestr(
                "pycyber-1.0.5.dist-info/METADATA",
                "Name: pycyber\nVersion: 1.0.5\n",
            )
        sdist = wheelhouse / "pycyber-1.0.5.tar.gz"
        metadata = self.repo / "PKG-INFO"
        metadata.write_text("Name: pycyber\nVersion: 1.0.4\n", encoding="utf-8")
        with tarfile.open(sdist, "w:gz") as archive:
            archive.add(metadata, arcname="pycyber-1.0.5/PKG-INFO")
            metadata.write_text("Name: pycyber\nVersion: 1.0.5\n", encoding="utf-8")
            archive.add(
                metadata, arcname="pycyber-1.0.5/staging/src/pycyber.egg-info/PKG-INFO"
            )
        result = subprocess.run(
            [
                sys.executable,
                str(self._script("check_release_version.py")),
                "--release",
                "--wheelhouse",
                str(wheelhouse),
            ],
            cwd=self.repo,
            env=self._env(MOCK_RELEASE_TAG="wheelos_core-v1.0.5"),
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("pycyber artifact metadata", result.stderr)

    def test_untagged_dev_artifacts_validate_name_and_embedded_version(self):
        wheelhouse = self.repo / "wheelhouse"
        wheelhouse.mkdir()
        version = "1.0.5.dev144+gdeadbee"
        wheel = wheelhouse / f"pycyber-{version}-py3-none-any.whl"
        command = [
            sys.executable, str(self._script("check_release_version.py")),
            "--release", "--wheelhouse", str(wheelhouse),
        ]
        with zipfile.ZipFile(wheel, "w") as archive:
            archive.writestr(
                f"pycyber-{version}.dist-info/METADATA",
                "Name: pycyber\nVersion: 1.0.4\n",
            )
        result = subprocess.run(command, cwd=self.repo, env=self._env(),
                                capture_output=True, text=True)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("pycyber artifact metadata", result.stderr)
        with zipfile.ZipFile(wheel, "w") as archive:
            archive.writestr(
                f"pycyber-{version}.dist-info/METADATA",
                f"Name: pycyber\nVersion: {version}\n",
            )
        sdist = wheelhouse / f"pycyber-{version}.tar.gz"
        metadata = self.repo / "PKG-INFO"
        metadata.write_text("Name: pycyber\nVersion: 1.0.4\n", encoding="utf-8")
        with tarfile.open(sdist, "w:gz") as archive:
            archive.add(metadata, arcname=f"pycyber-{version}/PKG-INFO")
        result = subprocess.run(command, cwd=self.repo, env=self._env(),
                                capture_output=True, text=True)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("pycyber artifact metadata", result.stderr)
        metadata.write_text(
            f"Name: pycyber\nVersion: {version}\n", encoding="utf-8"
        )
        with tarfile.open(sdist, "w:gz") as archive:
            archive.add(metadata, arcname=f"pycyber-{version}/PKG-INFO")
            metadata.write_text("Name: pycyber\nVersion: 1.0.4\n", encoding="utf-8")
            archive.add(
                metadata, arcname=f"pycyber-{version}/staging/src/pycyber.egg-info/PKG-INFO"
            )
        result = subprocess.run(command, cwd=self.repo, env=self._env(),
                                capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            version,
            subprocess.run(
                [sys.executable, str(self._script("check_release_version.py")),
                 "--python-version"],
                cwd=self.repo, env=self._env(), capture_output=True, text=True,
                check=True,
            ).stdout.strip(),
        )

    def test_publish_requires_matching_tag_ref_and_override(self):
        command = [
            sys.executable, str(self._script("check_release_version.py")),
            "--publish",
        ]

        def verify(**env):
            return subprocess.run(command, cwd=self.repo, env=self._env(**env),
                                  capture_output=True, text=True)

        self.assertIn("publish requires", verify().stderr)
        self.assertIn(
            "clean worktree",
            verify(MOCK_RELEASE_TAG="wheelos_core-v1.0.5",
                   MOCK_GIT_DIRTY=" M BUILD").stderr,
        )
        self.assertIn(
            "publish ref",
            verify(MOCK_RELEASE_TAG="wheelos_core-v1.0.5",
                   GITHUB_REF="refs/tags/wheelos_core-v1.0.4").stderr,
        )
        self.assertIn(
            "PYCYBER_VERSION",
            verify(MOCK_RELEASE_TAG="wheelos_core-v1.0.5",
                   GITHUB_REF="refs/tags/wheelos_core-v1.0.5",
                   PYCYBER_VERSION="9.9.9").stderr,
        )
        self.assertEqual(
            0,
            verify(MOCK_RELEASE_TAG="wheelos_core-v1.0.5",
                   GITHUB_REF="refs/tags/wheelos_core-v1.0.5").returncode,
        )
        self.assertEqual(
            "9.9.9",
            subprocess.run(
                [sys.executable, str(self._script("check_release_version.py")),
                 "--python-version"], cwd=self.repo,
                env=self._env(PYCYBER_VERSION="9.9.9"),
                capture_output=True, text=True, check=True,
            ).stdout.strip(),
        )

    def test_publish_entrypoints_reject_untagged_head_before_build(self):
        for script in ("build_release_artifacts.sh",
                       "build_and_package_pycyber.sh"):
            with self.subTest(script=script):
                result = subprocess.run(
                    ["/bin/bash", str(self._script(script)), "--publish"],
                    cwd=self.repo, env=self._env(),
                    capture_output=True, text=True,
                )
                self.assertNotEqual(0, result.returncode)
                self.assertIn("publish requires", result.stderr)
        self.assertFalse((self.repo / "packaging" / "pycyber" / ".venv").exists())

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
        for target in ("//cyber/node:writer_test",
                       "//cyber/node:writer_reader_test",
                       "//cyber/data:channel_buffer_test"):
            self.assertIn(target, calls)
        self.assertNotIn("examples_regression_tests", calls)
        self.assertIn(
            f"--test_env=CYBER_RECORD_PLAY_FIXTURE={fixture}",
            calls,
        )

    def test_leak_checks_reject_possible_losses(self):
        self._install_fake_bazel()
        self._write_executable(
            "valgrind",
            """#!/bin/bash
printf '%s\\n' "$*" >> "$CALL_LOG"
case " $* " in
  *" --errors-for-leak-kinds=definite,indirect,possible "*) exit 1;;
  *) exit 0;;
esac
""",
        )
        for script in ("run_memory_leak_check.sh",
                       "run_cyber_memory_leak_matrix.sh"):
            with self.subTest(script=script):
                result = subprocess.run(
                    ["/bin/bash", str(self._script(script)),
                     "--outdir", str(self.repo / script)],
                    cwd=self.repo, env=self._env(),
                    capture_output=True, text=True,
                )
                self.assertNotEqual(0, result.returncode)
        calls = self.call_log.read_text(encoding="utf-8")
        self.assertEqual(
            6, calls.count("--errors-for-leak-kinds=definite,indirect,possible")
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

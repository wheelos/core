#!/usr/bin/env python3
"""Qualify once, then verify the exact candidate bytes before publication."""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import container_test

GATES = ("unit", "integration", "package", "e2e", "perf")


def artifact_hashes(candidate):
    files = [
        path
        for directory in ("core", "pycyber")
        for path in (candidate / directory).rglob("*")
        if path.is_file()
    ]
    for required in ("manifest.txt", "SHA256SUMS"):
        path = candidate / required
        if not path.is_file():
            raise ValueError(f"Missing candidate metadata: {path}")
        files.append(path)
    if (
        len(list((candidate / "core").glob("*.deb"))) != 1
        or not list((candidate / "pycyber").glob("*.whl"))
        or not list((candidate / "pycyber").glob("*.tar.gz"))
    ):
        raise ValueError("Candidate requires one native deb, wheel and sdist")
    hashes = {}
    for path in sorted(files):
        if path.is_symlink():
            raise ValueError(f"Candidate cannot contain artifact symlinks: {path}")
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        hashes[str(path.relative_to(candidate))] = digest.hexdigest()
    return hashes


def verify(report, candidate, git_sha, publish=False):
    if report.get("schema_version") != 1:
        raise ValueError("Unsupported qualification schema")
    if report.get("overall_status") != "passed":
        raise ValueError("Release qualification is not complete")
    if report.get("git_sha") != git_sha:
        raise ValueError("Qualification belongs to a different commit")
    manifest = dict(
        line.split("=", 1)
        for line in (candidate / "manifest.txt").read_text().splitlines()
        if "=" in line
    )
    if (
        manifest.get("git_sha") != git_sha
        or not manifest.get("core_version")
        or not manifest.get("bazel_version")
    ):
        raise ValueError("Candidate manifest has missing or inconsistent provenance")
    if publish and report.get("source_dirty", True):
        raise ValueError("Publication requires a clean qualified source checkout")
    if report.get("long_run_seconds", 0) < 7200:
        raise ValueError("Two-hour stability gate is missing")
    gates = report.get("gates", {})
    if set(gates) != set(GATES) or any(
        gate.get("overall_status") != "passed"
        or gate.get("git_sha") != git_sha
        or not gate.get("image_id")
        for gate in gates.values()
    ):
        raise ValueError(
            "Qualification requires all five container gates at the same commit"
        )
    if (
        gates["e2e"].get("scope") != "full"
        or gates["perf"].get("long_run_seconds", 0) < 7200
        or gates["perf"].get("long_run_elapsed_seconds", 0) < 7200
        or len(
            {
                gates[name]["image_id"]
                for name in ("unit", "integration", "package", "perf")
            }
        )
        != 1
        or not report.get("source_fingerprint")
        or any(
            gate.get("source_fingerprint") != report["source_fingerprint"]
            for gate in gates.values()
        )
        or any(
            not gates[name].get("tests")
            or any(status != "PASSED" for status in gates[name]["tests"].values())
            for name in ("unit", "integration")
        )
    ):
        raise ValueError(
            "Qualification requires full package scope and unchanged sources"
        )
    if report.get("artifacts") != artifact_hashes(candidate):
        raise ValueError("Candidate artifacts changed after qualification")
    if gates["e2e"].get("tested_artifacts") != report["artifacts"]:
        raise ValueError("Installed E2E did not validate this complete candidate")


def verify_distributions(dist, reports, git_sha):
    qualified = set()
    for path in reports.rglob("container-report.json"):
        report = json.loads(path.read_text())
        if (
            report.get("stage") != "e2e"
            or report.get("overall_status") != "passed"
            or report.get("git_sha") != git_sha
            or not report.get("image_id")
        ):
            raise ValueError(f"Unqualified installation report: {path}")
        qualified.update(report.get("tested_artifacts", {}).values())
    distributions = list(dist.glob("*.whl")) + list(dist.glob("*.tar.gz"))
    if not distributions:
        raise ValueError("No distributions to publish")
    for path in distributions:
        with path.open("rb") as source:
            digest = hashlib.sha256(source.read()).hexdigest()
        if digest not in qualified:
            raise ValueError(
                f"Distribution has no exact installed E2E evidence: {path}"
            )


def git(*args):
    return subprocess.run(
        ["git", "-C", str(container_test.ROOT), *args],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("--distributions", type=Path)
    parser.add_argument("--installation-reports", type=Path)
    parser.add_argument(
        "--report",
        type=Path,
        default=container_test.ROOT / "artifacts/testing/qualification.json",
    )
    parser.add_argument(
        "--candidate",
        type=Path,
        default=container_test.ROOT / "artifacts/testing/candidate",
    )
    args, runner_args = parser.parse_known_args(argv)
    sha = git("rev-parse", "HEAD")
    if args.publish:
        subprocess.run(
            [
                sys.executable,
                str(container_test.ROOT / "scripts/release/check_release_version.py"),
                "--publish",
            ],
            cwd=container_test.ROOT,
            check=True,
        )
    if args.verify:
        if runner_args:
            parser.error("Runner arguments are not accepted in verify mode")
        verify(
            json.loads(args.report.read_text()),
            args.candidate,
            sha,
            args.publish,
        )
        if args.distributions:
            if not args.installation_reports:
                parser.error("--distributions requires --installation-reports")
            verify_distributions(args.distributions, args.installation_reports, sha)
        print("Qualification verified: same commit and exact artifact checksums")
        return
    dirty = bool(git("status", "--porcelain", "--untracked-files=normal"))
    report = {
        "schema_version": 1,
        "git_sha": sha,
        "source_dirty": dirty,
        "overall_status": "failed",
        "gates": {},
        "source_fingerprint": container_test.source_fingerprint(),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    command = ["all", "--artifacts", str(args.candidate), *runner_args]
    if args.publish:
        command.append("--publish")
    runner = container_test.parse_args(command)
    try:
        container_test.main(command)
        for gate in GATES:
            report["gates"][gate] = json.loads(
                (runner.outdir / gate / "container-report.json").read_text()
            )
        report["long_run_seconds"] = runner.long_run_seconds
        report["artifacts"] = artifact_hashes(args.candidate)
        if git("rev-parse", "HEAD") != sha:
            raise ValueError("Source commit changed during qualification")
        report["overall_status"] = "passed"
        verify(report, args.candidate, sha, args.publish)
    finally:
        if sys.exc_info()[0] is not None:
            report["overall_status"] = "failed"
            report["error"] = str(sys.exc_info()[1])
        args.report.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()

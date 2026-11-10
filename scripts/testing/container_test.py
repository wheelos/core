#!/usr/bin/env python3
"""Reproduce build-side integration and installed-package E2E in isolated Docker."""

import argparse
import fcntl
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
import uuid
from pathlib import Path

from artifacts import digest_file, write_python_metadata

ROOT = Path(__file__).resolve().parents[2]


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def source_files(root):
    return (
        subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "ls-files",
                "--cached",
                "--others",
                "--exclude-standard",
                "-z",
            ],
            capture_output=True,
            check=True,
        )
        .stdout.decode()
        .split("\0")
    )


def source_fingerprint(root=ROOT):
    digest = hashlib.sha256()
    for name in sorted(set(source_files(root)) - {""}):
        path = root / name
        digest.update(name.encode())
        if path.is_symlink():
            digest.update(os.readlink(path).encode())
        elif path.is_file():
            digest.update(digest_file(path).encode())
            digest.update(str(path.stat().st_mode & 0o111).encode())
        else:
            digest.update(b"deleted")
    return digest.hexdigest()


def git_state(root):
    return tuple(
        run(["git", "-C", str(root), *args], capture_output=True).stdout
        for args in (
            ("rev-parse", "HEAD"),
            ("ls-files", "--stage", "-z"),
            ("for-each-ref", "--format=%(refname) %(objectname)", "refs/tags"),
        )
    )


def prepare_workspace(cache, root=ROOT):
    root = root.resolve()
    fingerprint = source_fingerprint(root)
    state = git_state(root)
    key = hashlib.sha256(
        b"git-snapshot-v2\0" + fingerprint.encode() + b"\0" + b"\0".join(state)
    ).hexdigest()
    parent = cache.resolve() / "workspaces"
    parent.mkdir(parents=True, exist_ok=True)
    workspace = parent / key
    if not workspace.exists():
        with tempfile.TemporaryDirectory(prefix=".snapshot-", dir=parent) as temporary:
            staging = Path(temporary) / "source"
            run(
                [
                    "git",
                    "clone",
                    "--quiet",
                    "--local",
                    "--no-hardlinks",
                    "--no-checkout",
                    str(root),
                    str(staging),
                ]
            )
            run(
                [
                    "git",
                    "-C",
                    str(staging),
                    "update-ref",
                    "--no-deref",
                    "HEAD",
                    state[0].decode().strip(),
                ]
            )
            run(
                ["git", "-C", str(staging), "update-index", "-z", "--index-info"],
                input=state[1],
            )
            # Keep packaging's tag refresh local to this frozen repository.
            run(["git", "-C", str(staging), "remote", "set-url", "origin", "."])
            for name in set(source_files(root)) - {""}:
                source = root / name
                destination = staging / name
                if source.is_symlink():
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.symlink_to(os.readlink(source))
                elif source.is_file():
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)
            if source_fingerprint(root) != fingerprint or git_state(root) != state:
                raise RuntimeError("Source changed during snapshot creation; retry")
            if (
                source_fingerprint(staging) != fingerprint
                or git_state(staging) != state
            ):
                raise RuntimeError("Git snapshot does not match the source checkout")
            staging.rename(workspace)
    if source_fingerprint(workspace) != fingerprint or git_state(workspace) != state:
        raise RuntimeError(f"Cached source snapshot was modified: {workspace}")
    return workspace


def candidate_hashes(candidate):
    return {
        str(path.relative_to(candidate)): digest_file(path)
        for path in sorted(candidate.rglob("*"))
        if path.is_file()
    }


def install_candidate(staging, destination):
    if not staging.is_dir() or not any(staging.iterdir()):
        raise RuntimeError("Packaging produced no candidate")
    if destination.exists():
        if any(destination.iterdir()):
            destination.rename(
                destination.with_name(
                    destination.name + ".previous-" + uuid.uuid4().hex
                )
            )
        else:
            destination.rmdir()
    shutil.move(str(staging), str(destination))


def bazel_results(events, require_pass=True):
    results = {}
    for line in events.read_text().splitlines():
        event = json.loads(line)
        if "testSummary" in event:
            label = event["id"]["testSummary"]["label"]
            results[label] = event["testSummary"]["overallStatus"]
    if require_pass and (
        not results or any(status != "PASSED" for status in results.values())
    ):
        raise RuntimeError(f"Bazel test gate failed or had no results: {results}")
    return results


def docker_command(args, stage, name, image):
    cache = args.cache_dir.resolve()
    result = (args.outdir / stage).resolve()
    command = [
        "docker",
        "run",
        "--name",
        name,
        "--init",
        "--network",
        name,
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "--ipc=private",
        "--shm-size=1g",
        "--ulimit",
        "memlock=-1:-1",
        "--security-opt",
        "seccomp=unconfined",
        "--env",
        "HOME=/home/tester",
        "--mount",
        f"type=bind,src={result},dst=/results",
        "--mount",
        f"type=bind,src={cache / 'pip'},dst=/pip-cache",
        "--env",
        "PIP_CACHE_DIR=/pip-cache",
    ]
    if stage == "e2e":
        command += [
            "--tmpfs",
            f"/home/tester:uid={os.getuid()},gid={os.getgid()},mode=0700",
            "--env",
            "PYTHONNOUSERSITE=1",
            "--mount",
            f"type=bind,src={args.artifacts.resolve()},dst=/artifacts,readonly",
            "--mount",
            f"type=bind,src={args.fixtures.resolve()},dst=/fixtures,readonly",
            "--mount",
            f"type=bind,src={args.workspace / 'tests/e2e'},dst=/cases,readonly",
            "--workdir",
            "/tmp",
            image,
            "python3",
            "/cases/installed_runtime_test.py",
            "--artifacts",
            "/artifacts",
            "--fixtures",
            "/fixtures",
            "--outdir",
            "/results",
        ]
        if args.wheel_only:
            command.append("--wheel-only")
    else:
        command += [
            "--mount",
            f"type=bind,src={cache / 'home'},dst=/home/tester",
            "--mount",
            f"type=bind,src={args.workspace},dst=/workspace",
            "--mount",
            f"type=bind,src={cache / 'output'},dst=/cache/output",
            "--mount",
            f"type=bind,src={args.repository_cache.resolve()},dst=/cache/repository",
            "--mount",
            f"type=bind,src={args.disk_cache.resolve()},dst=/cache/disk",
            "--mount",
            f"type=bind,src={args.distdir.resolve()},dst=/cache/distdir",
            "--mount",
            f"type=bind,src={args.package_dir.resolve()},dst=/candidate",
            "--mount",
            f"type=bind,src={args.fixtures.resolve()},dst=/fixtures",
            "--workdir",
            "/workspace",
            "--env",
            f"LONG_RUN_SECONDS={args.long_run_seconds}",
            "--env",
            "DISTDIR=/cache/distdir",
            "--env",
            "PYTHONUSERBASE=/opt/python-userbase",
            "--env",
            f"WHEELOS_TEST_IMAGE_ID={image}",
            "--env",
            f"PUBLISH_RELEASE={'true' if args.publish else 'false'}",
            "--env",
            f"DIAGNOSTIC_TARGET={args.target or ''}",
            image,
            "bash",
            "scripts/testing/container_stage.sh",
            stage,
        ]
    return command


def execute(args, stage):
    if stage == "e2e":
        return execute_container(args, stage)
    args.cache_dir.mkdir(parents=True, exist_ok=True)
    with (args.cache_dir / "build-container.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return execute_container(args, stage)


def execute_container(args, stage):
    result = args.outdir / stage
    if (result / "container-report.json").exists():
        result.rename(result.with_name(result.name + ".previous-" + uuid.uuid4().hex))
    result.mkdir(parents=True, exist_ok=True)
    name = "wheelos-test-" + uuid.uuid4().hex[:12]
    args.package_dir = args.artifacts
    if stage in ("package", "wheel"):
        args.package_dir = args.cache_dir / "candidates" / name
        args.package_dir.mkdir(parents=True)
    image = args.runtime_image if stage == "e2e" else args.build_image
    metadata = {
        "schema_version": 1,
        "stage": stage,
        "git_sha": run(
            ["git", "-C", str(args.workspace), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
        ).stdout.strip(),
        "image": image,
        "uid": os.getuid(),
        "gid": os.getgid(),
        "started_at": time.time(),
        "overall_status": "failed",
        "source_fingerprint": source_fingerprint(args.workspace),
    }
    initial_artifacts = candidate_hashes(args.artifacts) if stage == "e2e" else None
    initial_fixtures = candidate_hashes(args.fixtures) if stage == "e2e" else None
    try:
        run(["docker", "network", "create", name], capture_output=True)
        metadata["image_id"] = run(
            ["docker", "image", "inspect", image, "--format", "{{.Id}}"],
            capture_output=True,
            text=True,
        ).stdout.strip()
        with (result / "container.log").open("w") as log:
            completed = subprocess.run(
                docker_command(args, stage, name, metadata["image_id"]),
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
                check=False,
            )
        metadata["exit_code"] = completed.returncode
        if stage in ("unit", "integration", "diagnostic"):
            events = result / "bazel-events.json"
            if events.exists():
                metadata["tests"] = bazel_results(events, require_pass=False)
                metadata["failed_targets"] = [
                    label
                    for label, status in metadata["tests"].items()
                    if status != "PASSED"
                ]
        if completed.returncode:
            raise RuntimeError(f"{stage} failed; see {result / 'container.log'}")
        if stage in ("unit", "integration", "diagnostic"):
            metadata["tests"] = bazel_results(result / "bazel-events.json")
        if stage == "e2e":
            report = json.loads((result / "report.json").read_text())
            if report["overall_status"] != "passed":
                raise RuntimeError("Package E2E did not pass")
        if stage == "perf":
            report = json.loads((result / "acceptance/report.json").read_text())
            if report["overall_status"] != "passed":
                raise RuntimeError("Performance/leak qualification did not pass")
            long_run = json.loads((result / "long-run.json").read_text())
            cases = [
                case for case in long_run["results"] if case["scenario"] == "long_run"
            ]
            if not cases or not all(
                case["success"]
                and case["reliability"]["total_loss"] == 0
                and case["duration_s"] >= args.long_run_seconds
                for case in cases
            ):
                raise RuntimeError(
                    "Long-running reliability gate failed or did not run"
                )
            if args.long_run_seconds < 7200:
                raise RuntimeError(
                    "Release stability gate requires at least 7200 seconds"
                )
            elapsed = json.loads((result / "long-run-timing.json").read_text())[
                "elapsed_seconds"
            ]
            if elapsed < args.long_run_seconds:
                raise RuntimeError("Measured long-run elapsed time is too short")
            metadata["long_run_elapsed_seconds"] = elapsed
            metadata["long_run_seconds"] = min(case["duration_s"] for case in cases)
        if source_fingerprint(args.workspace) != metadata["source_fingerprint"]:
            raise RuntimeError("Source files changed while the container was running")
        if stage == "wheel":
            write_python_metadata(
                args.package_dir / "release",
                {
                    "git_sha": metadata["git_sha"],
                    "bazel_version": "bazel "
                    + (args.workspace / ".bazelversion").read_text().strip(),
                },
            )
        if stage in ("package", "wheel"):
            install_candidate(args.package_dir / "release", args.artifacts)
        if stage == "e2e":
            metadata["scope"] = "wheel" if args.wheel_only else "full"
            metadata["tested_artifacts"] = candidate_hashes(args.artifacts)
            metadata["test_fixtures"] = initial_fixtures
            if metadata["tested_artifacts"] != initial_artifacts:
                raise RuntimeError("Candidate artifacts changed while E2E was running")
            if candidate_hashes(args.fixtures) != initial_fixtures:
                raise RuntimeError("Native fixtures changed while E2E was running")
        metadata["overall_status"] = "passed"
    except (
        OSError,
        ValueError,
        RuntimeError,
        subprocess.SubprocessError,
    ) as error:
        metadata["overall_status"] = "failed"
        metadata["error"] = str(error)
        raise
    finally:
        subprocess.run(
            ["docker", "stop", "--time", "10", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        state = subprocess.run(
            ["docker", "inspect", name],
            capture_output=True,
            text=True,
            check=False,
        )
        if state.returncode == 0:
            (result / "container-inspect.json").write_text(state.stdout)
        subprocess.run(
            ["docker", "rm", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        subprocess.run(
            ["docker", "network", "rm", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        metadata["finished_at"] = time.time()
        (result / "container-report.json").write_text(
            json.dumps(metadata, indent=2) + "\n"
        )


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "stage",
        choices=(
            "unit",
            "integration",
            "package",
            "wheel",
            "e2e",
            "perf",
            "all",
            "diagnostic",
        ),
    )
    parser.add_argument("--outdir", type=Path, default=ROOT / "artifacts/testing")
    parser.add_argument(
        "--artifacts", type=Path, default=ROOT / "artifacts/testing/candidate"
    )
    parser.add_argument(
        "--fixtures", type=Path, default=ROOT / "artifacts/testing/fixtures"
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path.home() / ".cache/wheelos-core-testing",
    )
    parser.add_argument("--repository-cache", type=Path)
    parser.add_argument("--target", help="Single Bazel target for a diagnostic rerun")
    parser.add_argument("--disk-cache", type=Path)
    parser.add_argument("--distdir", type=Path)
    parser.add_argument("--build-image", default="wheelos-core-test-build:local")
    parser.add_argument("--runtime-image", default="wheelos-core-test-runtime:local")
    parser.add_argument("--build-images", action="store_true")
    parser.add_argument(
        "--wheel-only",
        action="store_true",
        help="Validate only the Python deliverable; not full native qualification",
    )
    parser.add_argument(
        "--publish",
        action="store_true",
        help="Require a clean matching release tag for packaging",
    )
    parser.add_argument("--long-run-seconds", type=int, default=7200)
    parser.add_argument("--timeout", type=int, default=14400)
    args = parser.parse_args(argv)
    if (args.stage == "diagnostic") != bool(args.target):
        parser.error("--target is required only for the diagnostic stage")
    args.workspace = ROOT
    args.package_dir = args.artifacts
    args.repository_cache = args.repository_cache or args.cache_dir / "repository"
    args.disk_cache = args.disk_cache or args.cache_dir / "disk"
    args.distdir = args.distdir or args.cache_dir / "distdir"
    return args


def main(argv=None):
    args = parse_args(argv)
    if os.getuid() == 0:
        raise SystemExit("Run as the invoking non-root user; Bazel rejects root builds")
    candidate = args.artifacts.resolve()
    protected = (
        ROOT,
        Path.home(),
        args.cache_dir.resolve(),
        args.outdir.resolve(),
        args.fixtures.resolve(),
    )
    if any(path == candidate or candidate in path.parents for path in protected):
        raise SystemExit(
            "Candidate output must not replace a source, home, cache or report root"
        )
    for path in (
        args.outdir,
        args.artifacts,
        args.fixtures,
        args.cache_dir / "home",
        args.cache_dir / "pip",
        args.cache_dir / "output",
        args.repository_cache,
        args.disk_cache,
        args.distdir,
    ):
        path.mkdir(parents=True, exist_ok=True)
    stages = (
        ("unit", "integration", "package", "e2e", "perf")
        if args.stage == "all"
        else (args.stage,)
    )
    targets = {"runtime" if stage == "e2e" else "build" for stage in stages}
    for target in sorted(targets):
        image = args.runtime_image if target == "runtime" else args.build_image
        exists = (
            subprocess.run(
                ["docker", "image", "inspect", image],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            ).returncode
            == 0
        )
        if args.build_images or not exists:
            run(
                [
                    "docker",
                    "build",
                    "--target",
                    target,
                    "--build-arg",
                    f"TEST_UID={os.getuid()}",
                    "--build-arg",
                    f"TEST_GID={os.getgid()}",
                    "--build-arg",
                    f"BAZEL_VERSION={(ROOT / '.bazelversion').read_text().strip()}",
                    "-t",
                    image,
                    "-f",
                    str(ROOT / "docker/test.dockerfile"),
                    str(ROOT),
                ]
            )
    with (args.cache_dir / "build-container.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        args.workspace = prepare_workspace(args.cache_dir)
    for stage in stages:
        execute(args, stage)
        print(f"{stage}: passed ({args.outdir / stage})", flush=True)


if __name__ == "__main__":
    main()

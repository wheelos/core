#!/usr/bin/env python3
"""Export an immutable Python-only candidate without rebuilding distributions."""

import argparse
import hashlib
import shutil
import tempfile
from pathlib import Path


def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def python_distributions(candidate):
    wheels = sorted((candidate / "pycyber").glob("*.whl"))
    sdists = sorted((candidate / "pycyber").glob("*.tar.gz"))
    if len(wheels) != 1 or len(sdists) > 1:
        raise ValueError("Python candidate requires one wheel and at most one sdist")
    files = wheels + sdists
    if any(path.is_symlink() or not path.is_file() for path in files):
        raise ValueError("Distributions must be regular files")
    return files


def write_python_metadata(candidate, provenance):
    files = python_distributions(candidate)
    if not provenance.get("git_sha") or not provenance.get("bazel_version"):
        raise ValueError("Python candidate requires Git and Bazel provenance")
    metadata = dict(provenance, scope="wheel")
    (candidate / "manifest.txt").write_text(
        "".join(f"{key}={value}\n" for key, value in sorted(metadata.items()))
    )
    (candidate / "SHA256SUMS").write_text(
        "".join(
            f"{digest_file(path)}  {path.relative_to(candidate).as_posix()}\n"
            for path in files
        )
    )


def export_python_candidate(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    files = python_distributions(source)
    provenance = dict(
        line.split("=", 1)
        for line in (source / "manifest.txt").read_text().splitlines()
        if "=" in line
    )
    checksums = {}
    for line in (source / "SHA256SUMS").read_text().splitlines():
        digest, relative = line.split(maxsplit=1)
        checksums[relative.lstrip("*")] = digest
    for path in files:
        if checksums.get(path.relative_to(source).as_posix()) != digest_file(path):
            raise ValueError(f"Distribution changed before export: {path.name}")
    if destination.exists():
        raise FileExistsError(f"Candidate already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".python-candidate-", dir=destination.parent
    ) as temporary:
        staging = Path(temporary) / "candidate"
        (staging / "pycyber").mkdir(parents=True)
        for path in files:
            copied = staging / "pycyber" / path.name
            shutil.copy2(path, copied)
            if digest_file(copied) != checksums[path.relative_to(source).as_posix()]:
                raise ValueError(f"Distribution changed during export: {path.name}")
        write_python_metadata(staging, provenance)
        staging.rename(destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    export_python_candidate(args.source, args.destination)

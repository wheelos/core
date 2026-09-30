#!/usr/bin/env python3
"""Check that release identities agree without changing development builds."""

import argparse
from email.parser import BytesParser
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import zipfile


def version_from_rule(path, rule, name):
    text = path.read_text(encoding="utf-8")
    matches = re.findall(rf"\b{rule}\s*\((.*?)\n\)", text, re.DOTALL)
    versions = []
    for body in matches:
        if re.search(rf'\bname\s*=\s*"{re.escape(name)}"', body):
            versions.extend(re.findall(r'\bversion\s*=\s*"([^"]+)"', body))
    if len(versions) != 1:
        raise ValueError(f"expected one {name} version in {path}")
    return versions[0]


def check_python_metadata(artifact, version):
    if artifact.suffix == ".whl":
        with zipfile.ZipFile(artifact) as wheel:
            metadata_files = [
                name for name in wheel.namelist()
                if name.endswith(".dist-info/METADATA")
            ]
            if len(metadata_files) != 1:
                raise ValueError(f"expected one wheel METADATA in {artifact}")
            metadata = wheel.read(metadata_files[0])
    else:
        with tarfile.open(artifact, "r:gz") as sdist:
            metadata_files = [
                member for member in sdist.getmembers()
                if member.name == f"pycyber-{version}/PKG-INFO" and member.isfile()
            ]
            if len(metadata_files) != 1:
                raise ValueError(f"expected one sdist PKG-INFO in {artifact}")
            metadata = sdist.extractfile(metadata_files[0]).read()
    fields = BytesParser().parsebytes(metadata, headersonly=True)
    if fields.get("Name", "").lower() != "pycyber" or fields.get("Version") != version:
        raise ValueError(
            f"pycyber artifact metadata {fields.get('Name')} "
            f"{fields.get('Version')} differs from pycyber {version}: {artifact}"
        )


def git_output(repo, *args):
    return subprocess.run(
        ["git", *args], cwd=repo, check=True, capture_output=True, text=True
    ).stdout.strip()


def check(repo, release, deb, wheelhouse, publish=False):
    module_version = version_from_rule(
        repo / "MODULE.bazel", "module", "wheelos_core"
    )
    package_version = version_from_rule(repo / "BUILD", "pkg_deb", "wheelos_core")
    if module_version != package_version:
        raise ValueError(
            f"Debian package version {package_version} differs from "
            f"MODULE.bazel version {module_version}"
        )

    if deb is not None:
        if not re.fullmatch(
            rf"wheelos_core_{re.escape(module_version)}_[^/]+\.deb", deb.name
        ):
            raise ValueError(f"unexpected Debian artifact for {module_version}: {deb}")
        metadata = [
            subprocess.run(
                ["dpkg-deb", "-f", str(deb), field],
                cwd=repo,
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
            for field in ("Package", "Version")
        ]
        if metadata != ["wheelos_core", module_version]:
            raise ValueError(
                f"Debian artifact metadata {metadata} differs from "
                f"wheelos_core {module_version}"
            )

    python_version = None
    if release or wheelhouse is not None or publish:
        tags = git_output(
            repo, "tag", "--points-at", "HEAD", "--list", "wheelos_core-v*"
        ).splitlines()
        if tags and tags != [f"wheelos_core-v{module_version}"]:
            raise ValueError(
                f"release tag(s) {tags} differ from wheelos_core-v{module_version}"
            )
        if publish and tags != [f"wheelos_core-v{module_version}"]:
            raise ValueError(f"publish requires wheelos_core-v{module_version} at HEAD")
        if publish and git_output(
            repo, "status", "--porcelain", "--untracked-files=normal"
        ):
            raise ValueError("publish requires a clean worktree")
        if publish and os.environ.get("GITHUB_REF") and os.environ["GITHUB_REF"] != (
            f"refs/tags/wheelos_core-v{module_version}"
        ):
            raise ValueError(
                f"publish ref {os.environ['GITHUB_REF']} differs from "
                f"wheelos_core-v{module_version}"
            )
        override = os.environ.get("PYCYBER_VERSION")
        if tags:
            if override and override != module_version:
                raise ValueError(
                    f"PYCYBER_VERSION {override} differs from tagged release "
                    f"{module_version}"
                )
            python_version = module_version
        elif override:
            python_version = override
        else:
            count = git_output(repo, "rev-list", "--count", "HEAD")
            sha = git_output(repo, "rev-parse", "--short", "HEAD")
            python_version = f"{module_version}.dev{count}+g{sha}"
        if wheelhouse is not None:
            wheels = list(wheelhouse.glob("pycyber-*.whl"))
            if not wheels:
                raise ValueError(f"no pycyber wheel in {wheelhouse}")
            artifacts = wheels + list(wheelhouse.glob("pycyber-*.tar.gz"))
            for artifact in artifacts:
                if artifact.suffix == ".whl":
                    valid_name = artifact.name.startswith(
                        f"pycyber-{python_version}-"
                    )
                else:
                    valid_name = artifact.name == f"pycyber-{python_version}.tar.gz"
                if not valid_name:
                    raise ValueError(
                        f"unexpected pycyber artifact for {python_version}: "
                        f"{artifact.name}"
                    )
                check_python_metadata(artifact, python_version)
    return module_version, python_version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--publish", action="store_true",
                        help="require a matching tag at HEAD before publishing")
    parser.add_argument("--python-version", action="store_true",
                        help="print the pycyber version for this commit")
    parser.add_argument("--deb", type=Path)
    parser.add_argument("--wheelhouse", type=Path)
    args = parser.parse_args()
    try:
        core_version, python_version = check(
            args.repo_root, args.release or args.python_version,
            args.deb, args.wheelhouse, args.publish
        )
        print(python_version if args.python_version else core_version)
    except (ValueError, OSError, subprocess.CalledProcessError,
            zipfile.BadZipFile, tarfile.TarError) as exc:
        print(f"release version check failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

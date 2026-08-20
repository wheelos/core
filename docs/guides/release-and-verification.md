# Release and Verification

This guide covers the release workflow for producing deployable artifacts and then validating them as a user would receive them.

## Goals

The release process should verify two things:

1. the repository builds correctly in a clean environment
2. the generated package and wheel install and work when unpacked or imported in isolation

## Release artifact types

The project produces two main outputs:

- native runtime bundle: `wheelos_core` Debian package
- Python distribution: `pycyber` wheel and source distribution

The repository’s release flow is centered on:

```bash
bash scripts/release/build_release_artifacts.sh
```

This collects:

- native package outputs under `artifacts/release/core`
- Python package outputs under `artifacts/release/pycyber`
- a manifest and checksum metadata bundle

## Build the release artifacts

From the repository root:

```bash
bash scripts/release/build_release_artifacts.sh
```

This is the standard release entrypoint for packaging and artifact generation.

## Validate the native bundle

Build the package first:

```bash
bazel build //:wheelos_core
DEB="$(find bazel-bin -maxdepth 1 -name 'wheelos_core_*.deb' -print -quit)"
sudo apt install "$DEB"
source /opt/wheelos_core/setup.bash
```

Then verify the installed commands:

```bash
cyber_launch --help
mainboard --help
cyber_monitor --help
cyber_recorder play --help
```

The expected behavior is that the runtime commands start successfully and print usage information without crashing.

## Validate the Python package

Build the Python wheelhouse:

```bash
bash scripts/release/build_and_package_pycyber.sh
```

Then install and smoke-test the wheel:

```bash
python3 scripts/release/validate_pycyber.py \
  --python python3 \
  --wheel "$(find packaging/pycyber/wheelhouse -maxdepth 1 -name '*.whl' -print -quit)"
```

## Clean-container validation

For release-grade validation, prefer the clean Ubuntu 22.04 validation path documented in:

- [Release validation](release-validation.md)

That flow uses Docker to validate the exact package and wheel workflow as a non-root user in an isolated environment. This is stronger than checking the host build alone.

## Release checklist

Before publishing, confirm all of the following:

- `check_bzlmod_lockfile.sh --check` passes
- the Bazel baseline succeeds
- `//:wheelos_core` builds successfully
- the Debian runtime bundle installs cleanly
- Python wheel import smoke tests pass
- runtime commands respond to `--help`
- example startup works from a fresh runtime install

## Recommended validation order

1. local Bazel build
2. target smoke tests
3. packaged runtime validation
4. Python wheel validation
5. clean-container release validation

## Reference files

- `scripts/release/build_release_artifacts.sh`
- `scripts/release/build_and_package_pycyber.sh`
- `scripts/release/check_bzlmod_lockfile.sh`
- `scripts/release/validate_pycyber.py`
- `docs/guides/release-validation.md`

## Next steps

- [Release validation](release-validation.md)
- [Performance testing](performance-testing.md)
- [Secondary development and integration](secondary-development.md)

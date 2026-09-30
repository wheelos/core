# Build and release architecture

## Goals

1. Keep the repository Bzlmod-first and lockfile-driven.
2. Make Ubuntu 22.04 the reproducible baseline for build, regression, and release packaging.
3. Treat release packaging as an extension of the validated Bazel graph, not an ad-hoc side path.

## Canonical entrypoints

- `bash scripts/build.sh` — default developer build for `//cyber/...`.
- `bash scripts/release/check_bzlmod_lockfile.sh --check` — CI-safe lockfile verification.
- `bash scripts/release/check_bzlmod_lockfile.sh --update` — explicit lockfile refresh after dependency changes.
- `bash scripts/release/ubuntu2204_baseline.sh` — canonical compile + regression baseline.
- `bash scripts/release/build_release_artifacts.sh` — release-oriented artifact collection for `wheelos_core` and `pycyber`.
- `bash scripts/release/build_and_package_pycyber.sh` — Python wheel-focused release path.
- `//tests/integration_test:core_tool_matrix_tests` — shared runtime, tools,
  transport, Python, examples, and record/play acceptance target.

## Artifact model

- Native packaging is anchored on `//:wheelos_core`.
- Native artifact validation and collection use the same `--config=ci` deb
  output; validation must not rebuild or query a different configuration.
- Python packaging is anchored on Bazel-built extension modules plus staged Python sources and generated protobuf output.
- Release artifacts should be assembled only after the Ubuntu 22.04 baseline passes.
- `MODULE.bazel` is the release version input. `check_release_version.py`
  rejects mismatched Debian versions or tagged release identities before
  packaging and checks built Debian names/metadata and every pycyber
  artifact's filename and embedded wheel/sdist metadata. Untagged builds
  derive a `.dev` version unless `PYCYBER_VERSION` explicitly overrides it;
  overrides are development-only and cannot publish without a matching tag
  and release version.
- Native and Python artifact scripts support `--publish` to require the
  matching `wheelos_core-v<version>` tag at HEAD and a clean worktree. CI enables it for tagged
  wheel builds and checks the downloaded wheels again before PyPI publication;
  ordinary development builds do not need tags. A development manifest's
  `git_sha` identifies HEAD, not any uncommitted changes in the built package.
- `//scripts/release:release_scripts_test` exercises mismatch and artifact
  guards in a Bazel sandbox. The Ubuntu baseline also runs Writer/Reader and
  queue characterization tests alongside the existing runtime matrix; the
  pycyber release workflow runs the script tests before accepting artifacts.

## Lockfile policy

- Check in `MODULE.bazel.lock`.
- Use `--lockfile_mode=error` in CI and baseline validation.
- Use `--lockfile_mode=update` only in an intentional dependency-refresh step.

## CI policy

- `c-cpp.yml` should run the baseline script directly instead of duplicating Bazel commands inline.
- `release-pycyber.yml` should verify the lockfile before building wheel artifacts.
- Release jobs should keep `fetch-depth: 0` so `setuptools_scm` can resolve tag-derived versions.

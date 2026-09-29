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

## Lockfile policy

- Check in `MODULE.bazel.lock`.
- Use `--lockfile_mode=error` in CI and baseline validation.
- Use `--lockfile_mode=update` only in an intentional dependency-refresh step.

## CI policy

- `core-ci.yml` runs the Ubuntu 22.04 compile and regression baseline; configure it as a required PR check.
- `lint.yml` checks changed files; configure it as a required PR check and expand it to the full tree after existing lint debt is addressed.
- `release-pycyber.yml` builds and validates wheels for relevant PRs, and publishes only on `wheelos_core-v*` tags.
- Release jobs should keep `fetch-depth: 0` so `setuptools_scm` can resolve tag-derived versions.

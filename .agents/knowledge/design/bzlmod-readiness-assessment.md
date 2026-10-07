# Bzlmod readiness assessment (2026-06-14, updated)

## Scope

This note assesses:
1. Whether rules under `bazel/` can be replaced by official upstream rules and removed.
2. Whether the repository currently satisfies practical Bzlmod requirements.

## Migration applied

The following cleanup has been applied in this repository:

- Removed `WORKSPACE`.
- Removed `bazel/` custom rule files:
  - `bazel/python_rules.bzl`
  - `bazel/cpplint.bzl`
  - `bazel/install/install.bzl`
- Updated scripts to stop relying on removed paths:
  - `scripts/format/buildify.sh` no longer scans `bazel/` or root `WORKSPACE`.
  - `scripts/lint/lint.sh` no longer restores `WORKSPACE` from `WORKSPACE.source`.
  - `scripts/deps/installer_base.sh` no longer treats `WORKSPACE` as a Bazel root file.
  - `scripts/env/build.bash` comment updated to remove stale `bazel/third_party/...` path.

## Post-migration findings

### `bazel/` directory rules

- `bazel/` has been removed and there are no in-repo `load("//bazel...")` references.
- Protobuf BUILD files are using `@com_google_protobuf//bazel:*_proto_library.bzl`.

## Findings: Bzlmod readiness

### Good baseline

- `MODULE.bazel` is present and defines direct dependencies and required overrides.
- `.bazelrc` enables Bzlmod globally (`common --enable_bzlmod`).
- Lockfile policy is defined (`--lockfile_mode=error` for check, `--lockfile_mode=update` for refresh).
- Legacy root `WORKSPACE` has been removed.

### Current blockers / gaps (verification environment)

- `bash scripts/release/check_bzlmod_lockfile.sh --update` did not complete in the current environment due prolonged external dependency fetch/analysis stalls.
- `bazel mod deps --lockfile_mode=update` also did not complete within practical runtime in the same environment.
- External dependency availability is unstable in current environment (multiple mirror 404/timeouts observed), which currently blocks a clean end-to-end lockfile validation result.

## Practical conclusion

- Repository structure now matches Bzlmod-first practice at root level (`MODULE.bazel` only; no `WORKSPACE`; no repo-local `bazel/` rule shims).
- Final “fully verified” status is still gated by dependency fetch reliability in the current network/mirror environment.

## Recommended next steps

1. Re-run `bash scripts/release/check_bzlmod_lockfile.sh --update` and then `--check` in a stable network/mirror environment; commit `MODULE.bazel.lock` changes.
2. Keep proto BUILD generation logic aligned with official protobuf rule loads.
3. Remove any remaining repo-local `cpplint()`/`install()` compatibility shims from BUILD files and scripts.

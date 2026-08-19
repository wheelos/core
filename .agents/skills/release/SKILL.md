# Release

## When

Read this when building release packages, changing release workflows, or
validating deliverables.

## Rules / Facts

- The standard order is lockfile check, Ubuntu 22.04 baseline, and release
  artifact build; detailed commands and prerequisites are in
  `build-release-validation/SKILL.md`.
- A pycyber release also requires a verified Python, auditwheel, and patchelf
  environment.
- Before delivery, confirm that the native package, pycyber wheelhouse,
  checksums, and manifest were generated.
- Separate acceptance into package delivery/customer installation and runtime
  tools/examples execution. Both parts must be reported; package installation
  success does not imply that tools or examples run successfully.
- Report lockfile, baseline, native build, pycyber, auditwheel, and smoke-test
  results separately; partial success is not full release success.
- Keep `MODULE.bazel.lock` synchronized with the Bzlmod configuration; do not
  bypass problems by disabling lockfile checks.

## Sources

- `.agents/skills/build-release-validation/SKILL.md`
- `MODULE.bazel.lock`
- `scripts/release/check_bzlmod_lockfile.sh`
- `scripts/release/ubuntu2204_baseline.sh`
- `scripts/release/build_release_artifacts.sh`
- `.github/context/design/build-release-architecture.md`

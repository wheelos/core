# Troubleshooting

## When

Read this when builds, dependencies, runtime environments, releases, or lint
checks fail.

## Rules / Facts

- The first build requires the repository's Bazel/Bzlmod registries; if the
  build environment is missing, run `sudo bash scripts/deploy/build.sh`.
- For lockfile, baseline, or artifact failures, isolate each stage in the
  order described by `.agents/skills/release/SKILL.md`; do not treat partial
  success as release success.
- Source `scripts/env/runtime.bash` before running tools from `bazel-bin`.
- The lint entry point depends on `scripts/deps/installer_base.sh`; missing
  `flake8` is an explicit failure, while missing Bazel or buildifier causes the
  corresponding C++ check to be skipped.
- The Ubuntu baseline covers `//cyber`, `//:wheelos_core`, and integration
  regression; distinguish compile, test, and environment failures.
- Fast-DDS exit exceptions (such as heap-use-after-free or dangling proxy access
  during Domain::removeParticipant) indicate a reader/writer destruction ordering
  inversion; ensure Subscribers are removed before Publishers and that
  `Domain::removeParticipant` is called only after user endpoints are cleared.
- Offline builds use the vendor workflow and must keep the lockfile, vendor
  tree, and download-disabled parameters consistent.

## Sources

- `.bazelrc`
- `scripts/deploy/build.sh`
- `scripts/env/runtime.bash`
- `scripts/lint/lint.sh`
- `scripts/release/ubuntu2204_baseline.sh`
- `scripts/release/build_vendor_bundle.sh`
- `.github/context/skills/offline-vendor-validation.md`

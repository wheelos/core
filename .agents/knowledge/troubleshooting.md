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
- `scripts/lint/lint.sh` does not build or test with Bazel; its C++/Bazel checks
  require clang-format and Buildifier, Python checks require Black, isort, and
  Flake8, and shell checks require ShellCheck. Missing tools fail explicitly.
- Bazel Python tests use the hermetic Python toolchain; pass `PYTHONUSERBASE`
  through `--test_env` when tests need packages installed for that interpreter.
- GitHub lint checks changed files; use `bash scripts/lint/lint.sh --all` for a
  full-tree check. Full-tree checks may surface pre-existing lint debt.
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

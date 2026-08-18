# Build and release validation

## When

Read this when validating builds, the Ubuntu baseline, or complete release
artifacts.

## Rules / Facts

- Run from the repository root with the repository's Bazel 7/Bzlmod
  configuration. Use Python 3.10 for pycyber packaging; Linux auditwheel
  repair requires `patchelf >= 0.14.5`.
- Check the lockfile first:

   ```bash
   bash scripts/release/check_bzlmod_lockfile.sh --check
   ```

- Run the Ubuntu baseline:

   ```bash
   bash scripts/release/ubuntu2204_baseline.sh --distdir /tmp/cache/
   ```

- If a timing-sensitive integration test fails, rerun only the failed target
  once before deciding whether it is a reproducible regression.

- Build and package the complete release artifacts:

   ```bash
   PATH="$PWD/packaging/pycyber/.venv/bin:$PATH" \
     bash scripts/release/build_release_artifacts.sh --distdir /tmp/cache/
   ```

- Confirm that `artifacts/release/core/` contains the native package,
  `artifacts/release/pycyber/` contains a wheel, source archive, and
  `SHA256SUMS`, and `artifacts/release/manifest.txt` records the Git SHA and
  Bazel version.
- Report lockfile, baseline, native build, pycyber, auditwheel, and smoke-test
  results separately. Do not claim the complete release flow passed if
  auditwheel was skipped or the baseline required a rerun.

## Sources

- `scripts/release/check_bzlmod_lockfile.sh`
- `scripts/release/ubuntu2204_baseline.sh`
- `scripts/release/build_release_artifacts.sh`
- `packaging/pycyber/`

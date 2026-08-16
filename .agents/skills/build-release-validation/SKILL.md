# Build and release validation

Use this workflow to validate changes in `wheelos/core`.

## Preconditions

- Run from the repository root.
- Use Bazel 7 with the repository Bzlmod configuration.
- Use Python 3.10 for pycyber packaging.
- Linux auditwheel repair requires `patchelf >= 0.14.5`.

## Validation

1. Check the Bzlmod lockfile:

   ```bash
   bash scripts/release/check_bzlmod_lockfile.sh --check
   ```

2. Run the Ubuntu baseline:

   ```bash
   bash scripts/release/ubuntu2204_baseline.sh --distdir /tmp/cache/
   ```

   If a timing-sensitive integration test fails, rerun only the failed
   targets once before treating the result as a reproducible regression.

3. Build and package all release artifacts:

   ```bash
   PATH="$PWD/packaging/pycyber/.venv/bin:$PATH" \
     bash scripts/release/build_release_artifacts.sh --distdir /tmp/cache/
   ```

4. Confirm the manifest and artifacts:

   - `artifacts/release/core/` contains the native package.
   - `artifacts/release/pycyber/` contains a wheel, source archive, and
     `SHA256SUMS`.
   - `artifacts/release/manifest.txt` records the Git SHA and Bazel version.

## Result reporting

Report the lockfile, baseline, native build, pycyber packaging, auditwheel,
and smoke-test results separately. Do not claim the full release flow passed
when auditwheel is skipped or when the baseline only passed after rerunning
flaky targets.

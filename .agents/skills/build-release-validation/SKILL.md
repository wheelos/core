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

## Two-part acceptance model

Release validation has two independent acceptance parts. A release is
complete only when both parts pass, or when every exception is explicitly
recorded and accepted.

### Part 1: Package delivery and customer installation

Validate the artifacts a customer receives in a fresh x86_64 Ubuntu 22.04
container:

| Delivery | Artifact | Customer-side check |
| --- | --- | --- |
| Native runtime | `artifacts/release/core/*.deb` | Extract or install the deb, source `setup.bash`, and run `mainboard`, `cyber_recorder`, `cyber_monitor`, and `cyber_launch --help` |
| Python binary package | `artifacts/release/pycyber/*.whl` | Install into a fresh Python 3.10 venv and import `pycyber`, `pycyber.cyber`, `pycyber.record`, and `pycyber.proto.record_pb2` |
| Python source package | `artifacts/release/pycyber/*.tar.gz` | Install from the sdist into a fresh Python 3.10 venv and run the same imports |

Also verify `SHA256SUMS` and `manifest.txt`. A wheel validated with
`--skip-auditwheel` is only an x86_64 installation check; it is not a repaired
manylinux release.

### Part 2: Runtime, tools, and examples

Run the shipped runtime and examples in the same clean image, using the named
non-root `wheelos` user. The canonical runtime acceptance target is:

```bash
bazel test --config=ci --test_output=errors \
  //tests/integration_test:core_tool_matrix_tests
```

The Ubuntu baseline and release acceptance runner share this target. Do not
rerun its constituent suites separately in the same acceptance flow.

Set an unlimited memlock limit for runtime/example containers that exercise
io_uring or shared-memory paths:

```yaml
ulimits:
  memlock:
    soft: -1
    hard: -1
```

Use `--privileged` only when the target also requires other kernel or IPC
permissions. Keep Bazel analysis and hermetic Python builds as the
non-root `wheelos` user; running Bazel as root is rejected by `rules_python`.
If an io_uring example passes only as privileged root, record that as an
environment/UID requirement rather than silently changing the standard
non-root acceptance result.

For customer data or record/play examples, mount the data directory read-only:

```bash
-v /mnt/synology:/mnt/synology:ro
```

If a test expects a fixed fixture such as
`/mnt/synology/apollo/sensor_rgb.record`, map an existing fixture to that
container path without modifying the source data. Record/play, recorder
cross-process `info`, and Python record examples must be reported separately
from the general examples smoke result because they depend on io_uring,
filesystem, and process-lifecycle behavior.

The runtime result must distinguish:

- build/packaging success;
- package installation and import success;
- tools starting successfully;
- Python example success;
- C++/integration example success;
- environmental or fixture failures.

Do not convert an example or tool failure into a package pass, and do not
claim the full release flow passed when either acceptance part is incomplete.

## Sources

- `scripts/release/check_bzlmod_lockfile.sh`
- `scripts/release/ubuntu2204_baseline.sh`
- `scripts/release/build_release_artifacts.sh`
- `packaging/pycyber/`

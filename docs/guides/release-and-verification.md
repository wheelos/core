# Release and Verification

This is the canonical test ownership and release workflow. Integration tests
run in a build container; end-to-end tests consume the exact candidate packages
in a separate runtime-only container without the checkout or Bazel outputs.

## Test boundaries and ownership

| Layer | Responsibility | Accountable role | Trigger |
| --- | --- | --- | --- |
| `//tests:unit` | Module logic, boundaries, error handling and ownership | Changing module's maintainer | Every PR |
| `//tests:integration` | Module contracts, discovery, transport, scheduling, tools and process lifecycle | Maintainers of the participating modules | Every PR |
| `//tests:performance_regression` | Short deterministic performance-harness regressions, not release performance qualification | Performance maintainer | Every PR |
| Installed-package E2E | Independent processes using shipped native runtime and installed Python distributions | Release maintainer and scenario owner | Packaging PRs, nightly, release tags |
| Performance, stability and leaks | Required benchmark cases, a two-hour pub/sub run and both Valgrind matrices | Performance maintainer | Nightly and release tags |
| `//tests:hardware` | CUDA/NvSci hardware-specific contracts | GPU platform maintainer | Explicit qualified hardware runner |

Roles may be held by the same person, but every failed gate needs an identified
owner in the PR. The author supplies adjacent tests and reproduces failures;
CI maintainers own image/cache/report failures, not runtime correctness.
The release maintainer verifies the candidate evidence and controls delivery.

Test implementations remain in their nearest Bazel package. Assign new tests
to a layer in `tests/BUILD`; `check_test_layers.py` rejects unclassified or stale
explicit test rules. Hardware tests are not silently included in a CPU pass.
The existing fixture-dependent manual record test remains a separately invoked
test; its absence is not evidence that an external dataset was validated.

## Reproducible container entrypoints

Run as the current non-root user from the checkout:

```bash
python3 scripts/testing/container_test.py unit --build-images
python3 scripts/testing/container_test.py integration
python3 scripts/testing/container_test.py package
python3 scripts/testing/container_test.py e2e
```

`docker/test.dockerfile` supplies Ubuntu 22.04 runtime and build targets.
The builder pins Bazel from `.bazelversion` and verifies it before running.
Containers use the invoking UID/GID, private IPC, 1 GiB shared memory, unlimited
memlock and relaxed seccomp for io_uring, without privileged mode or host
networking. No host IPC resources are removed.

Caches default to `~/.cache/wheelos-core-testing`. Reuse existing downloads
and compiled actions explicitly:

```bash
python3 scripts/testing/container_test.py integration \
  --repository-cache /path/to/existing/repo_cache \
  --disk-cache /path/to/existing/disk_cache \
  --distdir /path/to/existing/distdir
```

Cache reuse does not mean a cold build was tested. Bazel output state is
separate from dependency/action caches. A source snapshot keyed by file contents
and Git HEAD, index and tags
keeps generated build files and packaging staging out of the invoking checkout;
changes during a run cannot silently alter that snapshot. Images are reused unless missing or
`--build-images` is requested. Rebuild them after changing the Dockerfile or
the pinned Bazel version.

Snapshots are independent local Git clones, including for linked worktrees;
they do not rely on host Git paths or hard-linked objects. Creation is atomic,
and committing or tagging previously tested contents selects a new snapshot.
Fetch required tags in the invoking checkout before starting: packaging inside
the snapshot only refreshes its frozen local references.

Containers sharing a mutable Bazel output cache are serialized by a host-side
lock. Separate caches may run concurrently and still share repository/action
caches. Test-result caching is keyed by the image digest; integration tests
always execute rather than accepting cached process/IPC results.

The package stage writes `artifacts/testing/candidate` and stages test fixtures
separately. It builds in a temporary output directory and preserves an existing
candidate as `candidate.previous-<id>` before replacing it after success.
Runtime logs go to the stage's results rather than dirtying the source snapshot.
E2E mounts the candidate, test harness, fixtures, pip dependency
cache and results, with a fresh temporary HOME;
it has no compiler, checkout, `bazel-bin`, Bazel cache or source PYTHONPATH.
Python dependencies may use the pip cache/network. Each stage has its own
Docker bridge network, separate from other test containers.
The runtime baseline includes `libatomic1`, also declared by the native deb;
the Linux sdist requires that system library alongside the C++ runtime.

Both full and Python-only candidates use the same directory contract:
`manifest.txt`, `SHA256SUMS`, and `pycyber/` distributions; full candidates also
contain `core/`. The `wheel` stage produces Python-only metadata. CI exports
the Python subset without rebuilding, and downloads that complete subset for
wheel-only E2E:

```bash
python3 scripts/testing/artifacts.py \
  artifacts/testing/candidate artifacts/testing/python-candidate
python3 scripts/testing/container_test.py e2e --wheel-only \
  --artifacts artifacts/testing/python-candidate
```

Export checks producer hashes, preserves distribution bytes, and writes a
checksum list covering only the exported files. Its destination must not
already exist. Metadata travels with each architecture's candidate; only
wheel/sdist files are collected for PyPI.

`--wheel-only` validates the Python deliverable, including independent process
communication, and must never be reported as native runtime qualification.
The native release baseline is x86_64; ARM wheel installation runs on native
ARM runners. GPU qualification requires its own compatible CUDA/NvSci runner
and is not implied by either CPU architecture's result.

Each stage retains its container log, image digest, UID/GID, exit status and
Docker state under `artifacts/testing/<stage>`. Bazel stages also preserve test
logs/XML and build events. E2E writes scenario-level `report.json`. Failures
propagate as nonzero exit codes; repeating a completed stage archives its prior
results as `<stage>.previous-<id>`, preserving failures and read-only Bazel logs.
A timing-sensitive failure may be rerun once without the test-result cache:

```bash
python3 scripts/testing/container_test.py diagnostic \
  --target //tests/integration_test:cross_process_churn_test \
  --outdir artifacts/testing/retry
```

Diagnostic reports cannot authorize any release gate.

## Mandatory release qualification

```bash
python3 scripts/testing/qualify_release.py --build-images
```

This runs unit, integration, packaging, installed-artifact E2E, performance,
Valgrind and two-hour stability gates in order. It writes
`artifacts/testing/qualification.json`, binding all gates to the commit and
the exact candidate file hashes. Development runs may use a dirty checkout,
but their evidence cannot authorize publication.

For a clean, matching `wheelos_core-v<version>` tag:

```bash
python3 scripts/testing/qualify_release.py --publish --build-images
python3 scripts/testing/qualify_release.py --verify --publish
```

Do not rebuild between qualification and publication. Verification rejects
missing gates, failed gates, short stability runs, a different commit, dirty
release sources, missing artifacts and modified or added artifact bytes.
The existing `run_release_acceptance.sh --stage ...` is useful for diagnosis,
but its requested-stage `passed` status is not a full release authorization.
Quick, skipped, unsupported and not-run gates are not full release passes.

## CI and delivery responsibilities

1. `WheelOS Core CI` runs unit/integration/short performance regression in
   containers for PRs and main pushes.
2. `PyCyber Verification and Release` builds candidates and runs installed
   E2E for relevant PRs, with native x86_64 and ARM wheel coverage.
3. `Nightly Release Qualification` executes all five gates, including the
   two-hour run and leak checks, and retains evidence for 14 days.
4. On release tags, full native qualification and installed wheel tests must
   complete before publishing. The publisher rechecks candidate hashes and
   each distribution's installation evidence, then publishes those same bytes
   to PyPI. Native GitHub Release delivery depends on PyPI success.

Repository administrators must configure required lint/core checks and the
`release` environment's reviewers and tag deployment restrictions. PyPI trusted
publishing must permit this workflow and its `release` environment. These are
server-side settings, not guarantees made by committing workflow YAML.
Never waive a failed gate by changing a workflow condition. If delivery fails
after a channel has published, preserve the successful channel's immutable
artifacts, investigate, and recover the other channel without rebuilding or
overwriting an existing version.

## Diagnostic references

The existing `build_release_artifacts.sh`, `validate_runtime_bundle.sh` and
`validate_pycyber.py` remain useful packaging helpers. Their individual
success does not replace full qualification.

- [Manual clean-container diagnostics](release-validation.md)
- [Performance testing](performance-testing.md)
- [Secondary development and integration](secondary-development.md)

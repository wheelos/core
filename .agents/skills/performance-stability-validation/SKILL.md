# Performance and stability validation

## When

Use this skill when a change must be validated in a clean container for both
performance and long-running runtime stability. It is the acceptance procedure
for source builds, pub/sub stress, packet/data integrity, and memory-leak
checks.

## Acceptance contract

The result has two independent gates:

1. **Performance gate:** the source tree builds in a fresh Ubuntu 22.04 x86_64
   container, the repository performance baseline completes, every reported
   case has `success=true`, and the report's loss counters/rates satisfy the
   release threshold agreed for the hardware. Do not invent an absolute
   latency or throughput threshold after the run; record the hardware and use
   a checked-in baseline or an explicitly approved threshold.
2. **Stability gate:** the long-run pub/sub case completes for 7200 seconds,
   receives the expected packets without payload mismatch or loss, and the
   bounded startup workload produces no Valgrind definite/possible leaks.
   A timeout, crash, OOM, packet mismatch, or leak is a failure.

Performance and stability results are reported separately. A passing build,
short benchmark, or smoke test alone is not a full pass.

## Clean container

Build the validation image from the repository root. Do not mount host
`bazel-bin`, `bazel-out`, or a host Bazel repository cache when claiming a
clean build:

```bash
docker build \
  --build-arg WHEELOS_UID="$(id -u)" \
  --build-arg WHEELOS_GID="$(id -g)" \
  -t wheelos-core-validation:x86_64 \
  -f docker/dev.x86_64.dockerfile .
```

Run as the non-root `wheelos` user. Use unlimited memlock for shared-memory,
io_uring, or RTPS tests. Use `--privileged` only when the selected transport
requires additional IPC/kernel permissions.

```bash
mkdir -p artifacts/validation
docker run --rm --user wheelos \
  -e HOME=/home/wheelos -e USER=wheelos \
  -v "$PWD:/workspace" -w /workspace \
  --network host --ipc private \
  --ulimit memlock=-1 \
  wheelos-core-validation:x86_64 \
  bash -lc 'set -euo pipefail; command -v bazel; bazel --version'
```

The validation image includes `valgrind` for the leak gate. Rebuild the image
if it predates this skill; never silently skip the leak gate.

## Performance gate

Inside the clean container, build from source and run the repository baseline:

```bash
set -euo pipefail
bash scripts/release/check_bzlmod_lockfile.sh --check
bazel build --config=ci --distdir=/tmp/cache //cyber //:wheelos_core
bash scripts/release/run_performance_baseline.sh \
  --outdir=artifacts/validation/performance --quick
```

For a release candidate, rerun without `--quick` and retain
`baseline.json`, `summary.md`, `benchmark.log`, and `metadata.txt`. Inspect
`baseline.json` rather than only the process exit code:

```bash
python3 - artifacts/validation/performance/baseline.json <<'PY'
import json
import sys

results = json.load(open(sys.argv[1], encoding="utf-8"))["results"]
assert results, "performance report contains no cases"
assert all(item["success"] for item in results), "performance case failed"
assert all(item["reliability"]["total_loss"] == 0 for item in results), \
    "performance run lost packets"
PY
```

Record `uname -a`, CPU, memory, transport configuration, container image
digest, Git SHA, Bazel version, and whether the run used host networking or
privileged IPC. Compare throughput and p99 latency only with measurements from
the same environment.

## Two-hour pub/sub stress gate

The benchmark monitor has a discovery-gated long-run case. Run it for exactly
two hours and write results to a separate directory:

```bash
mkdir -p artifacts/validation/stability
source scripts/env/runtime.bash
bazel build --config=ci //tests/perf_test:benchmark_monitor
timeout --foreground --kill-after=30s 2h \
  bazel-bin/tests/perf_test/benchmark_monitor \
  --output_json="$PWD/artifacts/validation/stability/long-run.json" \
  --enable_long_run=true \
  --long_run_seconds=7200 \
  --long_run_frequency_hz=200 \
  --long_run_payload_bytes=1024 \
  2>&1 | tee artifacts/validation/stability/long-run.log
```

The command must exit successfully, and the JSON must contain a successful
`long_run` result with zero `reliability.total_loss`. The benchmark's receive
path validates packet payloads before setting `success`; a discovery or
environment failure is recorded as such and is not converted into a pass.
Keep the stdout/stderr log and the JSON report.

## Offline vendor fallback

If a clean container cannot download Bazel dependencies, use a vendor archive
before classifying the result as a code failure. Mount only
`artifacts/vendor-clean` into a `--network none` container and choose the
archive generated with the same Bazel version and Git SHA:

```bash
docker run --rm --network none --user wheelos \
  -v "$PWD/artifacts/vendor-clean:/input:ro" \
  wheelos-core-validation:x86_64 bash -lc '
    set -euo pipefail
    rm -rf /tmp/vendor-work /tmp/offline-output /tmp/empty-repository-cache
    mkdir -p /tmp/vendor-work /tmp/offline-output /tmp/empty-repository-cache
    tar -xzf /input/wheelos_core_vendor_clean_<timestamp>.tar.gz \
      -C /tmp/vendor-work
    cd /tmp/vendor-work/repo
    test -f MODULE.bazel.lock
    test -d vendor/bazel/_registries
    rm -rf vendor/bazel/bazel-external
    bazel --output_user_root=/tmp/offline-output build \
      --config=ci --lockfile_mode=error \
      --vendor_dir=vendor/bazel \
      --repository_cache=/tmp/empty-repository-cache \
      --repository_disable_download \
      //tests/perf_test:benchmark_monitor
  '
```

The archive must contain `MODULE.bazel.lock`, registry files, repository
contents, and `VENDOR.bazel`; it must not contain
`vendor/bazel/bazel-external`. The extracted tree must be writable because
Bazel recreates that link. Reject the archive if its lockfile version or Git
SHA does not match the validation toolchain/source. Regenerate a matching
package with `scripts/release/build_vendor_bundle.sh` rather than using
`--lockfile_mode=update` or enabling downloads in the offline run.

For a packet/record-reader workload with an existing non-empty record file,
also run the bounded reader under Valgrind:

```bash
bash scripts/release/run_memory_leak_check.sh \
  --record-file /path/to/local-fixture.record \
  --mode uring_stream --max-chunks 1 \
  --outdir artifacts/validation/memory-leak
```

This starts the actual built program and fails on Valgrind definite or
possible leaks. `--record-file` must be on local storage for publish/read
performance claims; network filesystem runs are diagnostic only.

## Runtime and memory observations

During the long run, capture process health without changing the workload:

```bash
pgrep -af benchmark_monitor
pid="$(pgrep -n benchmark_monitor)"
grep -E 'VmRSS|VmHWM|Threads' "/proc/$pid/status"
```

Sample RSS and thread count at startup, 15 minutes, 1 hour, and completion.
Treat a monotonic unexplained RSS trend, thread growth, crash, OOM kill, or
stalled packet counter as a stability failure. Use Valgrind for leak
attribution; `/proc` RSS is an observation, not proof of a leak.

## Required artifacts and reporting

Store all output under one timestamped directory:

- `performance/{baseline.json,summary.md,benchmark.log,metadata.txt}`
- `stability/{long-run.json,long-run.log}`
- `memory-leak/{valgrind.log,metadata.txt}`

Report build, performance, long-run stability, packet integrity/loss, and leak
results as separate rows. Include the exact commands, Git SHA, image digest,
transport/configuration, fixture path and filesystem, resource limits, and
any environmental failure. Do not report stability as passed when the
two-hour run or leak gate was skipped.

## Sources

- `scripts/release/run_performance_baseline.sh`
- `scripts/release/run_memory_leak_check.sh`
- `scripts/release/run_cyber_memory_leak_matrix.sh`
- `tests/perf_test/cyber_rt_benchmark_suite.cc`
- `tests/integration_test:examples_regression_tests`
- `docs/guides/release-validation.md`

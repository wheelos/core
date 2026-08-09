# WheelOS Core release readiness

This checklist maps the release selling points to a reproducible artifact or
validation command. A release is complete only when the commands below pass
on the target hardware and their output is archived with the release.

## Performance

| Selling point | Evidence |
| --- | --- |
| Same-host zero-copy | `bash scripts/release/run_performance_baseline.sh --full`; inspect `summary.md` for `shm_zero_copy_probe`, `zero_copy_copy_count=0`, loss, p99 latency, throughput, CPU, and RSS. |
| Record data written/read from disk | `bash scripts/record_perf_reader.sh <record-file> 0 artifacts/performance/record-reader-<id>`; archive the four JSON mode reports, `benchmark.log`, `metadata.txt`, and optional `strace_*.txt`. |

The performance claims are hardware-specific. Publish the CPU, kernel,
transport configuration, payload sizes, duration, and Git SHA alongside the
numbers; do not present the checked-in example numbers as universal
guarantees.

## APIs and application scenarios

| API | Example | Typical application |
| --- | --- | --- |
| Publish/subscribe | `bazel run //examples:talker` and `//examples:listener` | Sensor streams, telemetry, perception and control pipelines. |
| Parameter service | `bazel run //examples:paramserver` or Python `//cyber/python/cyber_py3/examples:parameter` | Runtime configuration, calibration, and feature flags. |
| Server/client service | `bazel run //examples:service`; Python `service` and `client` | Request/response operations such as planning queries, health checks, and control commands. |
| Record/replay | `bazel run //examples:record` and the `cyber_recorder` tool | Offline debugging, regression replay, and dataset capture. |

The integration regression suite must pass before publishing:

```bash
bazel test --config=ci //tests/integration_test:examples_regression_tests \
  --test_output=errors
```

## Release artifacts and downstream consumption

Build the native runtime and Python artifacts from the same commit:

```bash
bash scripts/release/build_release_artifacts.sh
bash scripts/release/validate_downstream_bazel_sdk.sh
```

- Python: publish one `pycyber` wheel for `linux_x86_64` and one for
  `linux_aarch64`; install the matching wheel with `python3 -m pip install`.
- C++ runtime: publish the versioned Debian package and consume it as
  `/opt/wheelos_core` after `source /opt/wheelos_core/setup.bash`.
- C++ development: use the `wheelos_core` Bzlmod module and
  `@wheelos_core//cyber:cyber`; the Debian runtime package is not a header SDK.

Python architecture jobs are defined in
`.github/workflows/release-pycyber.yml`. The C++ package and independent
downstream SDK validation are covered by the release scripts.

## Memory safety

Run the focused leak gate on a machine with Valgrind installed:

```bash
bash scripts/release/run_memory_leak_check.sh
bash scripts/release/run_cyber_memory_leak_matrix.sh
```

The matrix covers pub/sub, fanout/fanin, multiple service clients, and
parameter server/client paths. It is not proof that every runtime path is
leak-free: add a reviewed target for each new long-running component or
transport path, and investigate failures before publishing. Tests that call
`_Exit` can retain framework allocations because normal teardown is skipped;
those reports must be repeated with an orderly shutdown before being
classified as third-party or process-lifetime allocations.

# WheelOS Core Release Baseline

The release baseline verifies three capabilities:

1. Cyber RT communication, services, scheduling, discovery, record I/O, and
   Python APIs work.
2. Runnable examples have consistent paths and pass automated smoke or
   integration tests.
3. Performance results are reproducible and archived for version comparisons.

## Architecture boundaries

`cyber/` contains the runtime and public APIs: initialization, nodes,
scheduling, discovery, transport, record I/O, mainboard, and Python bindings.
`examples/` is the user-facing example entry point. `tests/` contains unit,
integration, and performance validation. Runtime packages do not contain
duplicate examples.

```text
cyber/       Runtime and public APIs
examples/    Runnable pub/sub, service, record, and component examples
tests/       Integration and performance validation
scripts/     Build, release, and report entrypoints
```

## Functional release gates

The baseline uses Ubuntu 22.04, version-locked Bzlmod dependencies, and the
checked-in `MODULE.bazel.lock`. Run these commands before publishing:

```bash
bash scripts/release/check_bzlmod_lockfile.sh --check
bash scripts/release/ubuntu2204_baseline.sh --with-pycyber
bazel build --config=ci //examples/...
bazel test --config=ci //tests/integration_test:examples_regression_tests \
  --test_output=errors
```

`examples_regression_tests` covers lifecycle, mainboard error handling,
cross-process tool discovery, record playback, binary payloads, fanout,
fanin, payload stress, and service bursts. The Ubuntu baseline script runs
the core C++ and Python unit tests. New runtime functionality must add a
deterministic unit test in its corresponding `cyber/` package.

Every example must:

- provide a Bazel build target;
- include minimal run instructions;
- avoid machine-specific absolute paths such as `/apollo`;
- be covered by at least one smoke or integration test.

## Performance baseline

Performance does not share absolute thresholds with short functional tests.
The release archives structured JSON and compares results only with historical
results from the same hardware, operating system, and configuration:

```bash
bash scripts/release/run_performance_baseline.sh
```

Reports are written to `artifacts/performance/<UTC timestamp>/`:

- `baseline.json`: latency, throughput, loss, duplicates, CPU, RSS, and
  context switches;
- `summary.md`: readable results grouped by transport coverage and message
  type;
- `benchmark.log`: execution log;
- `metadata.txt`: Git SHA, Bazel version, kernel, and execution mode.

The default `--quick` mode is for release-candidate smoke testing. Use
`--full` for the complete matrix. Block a release if a case fails, a core
intra/SHM 1:1 case loses messages, resources grow unexpectedly, or results
regress materially against the previous equivalent baseline. Cross-host
simulations are for trend analysis, not absolute gates for the first stable
release.

### Large-message Protobuf and POD comparison

The performance script also compares same-host, cross-process 1:1 transfers:
SHM Protobuf and Iceoryx POD send 1, 4, and 7 MiB payloads at 30 Hz for
three seconds per point. The `Large-message comparison` table in `summary.md`
records p99 latency, MiB/s, loss rate, and POD zero-copy status. This validates
the same-host data plane and is not mixed with cross-host RTPS results.

An Iceoryx POD chunk is approximately 8 MiB, making 7 MiB the largest comparison
point in this baseline. Larger payloads should use a chunked protocol or RTPS.

#### Same-host large-message baseline on 2026-08-05

The results below are from `artifacts/performance/protobuf-pod-20260805/`.
Each case uses independent publisher and subscriber processes, 1:1 delivery,
30 Hz, and three seconds. Protobuf uses SHM; POD uses an Iceoryx loan. Neither
path had send failures or message loss.

| Payload | Protobuf p99 | POD p99 | Payload throughput | Protobuf CPU | POD CPU | Protobuf RSS | POD RSS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 MiB | 1.129 ms | 0.468 ms | 30 MiB/s | 4.27% | 1.43% | 392 MiB | 96 MiB |
| 4 MiB | 6.034 ms | 1.192 ms | 120 MiB/s | 7.40% | 2.20% | 594 MiB | 196 MiB |
| 7 MiB | 9.241 ms | 3.473 ms | 210 MiB/s | 9.33% | 3.02% | 793 MiB | 298 MiB |

POD completed 90 loaned publications per point with
`zero_copy_copy_count=0`. The baseline recommendation is to prefer POD/Iceoryx
for same-host large messages. Use Protobuf SHM when protobuf semantics are
required or payloads exceed the 8 MiB single-chunk limit, with chunking to
control memory usage. This is a first reference baseline; comparisons require
the same kernel, CPU affinity, Iceoryx configuration, and script arguments.

## Minimum release acceptance

A release candidate must have:

1. passing lockfile checks, baseline scripts, and example builds;
2. passing integration regression tests;
3. archived performance JSON, logs, and metadata;
4. installable and runnable native and Python artifacts from
   `build_release_artifacts.sh`.

The first stable release does not require a full coverage threshold, a
24-hour soak, or cross-host performance comparison. Those are later
enhancements and should not delay the initial stable baseline.

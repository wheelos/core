# Performance Testing

This guide explains how to run the repository’s built-in performance checks and how to interpret the results.

## Scope

The performance suite focuses on transport throughput, latency sensitivity, and zero-copy behavior for large payloads and sensor-like messages.

Relevant repository locations:

- `tests/perf_test/BUILD`
- `tests/perf_test/benchmark_pub.cc`
- `tests/perf_test/benchmark_sub.cc`
- `tests/perf_test/cyber_rt_benchmark_suite.cc`

## Benchmark categories

The target suite includes benchmark-style checks for:

- publish throughput
- subscription receive performance
- zero-copy verification
- sensor and POD-style payload handling
- SHM and same-host data path behavior

## Build the benchmark targets

```bash
bazel build //tests/perf_test:benchmark_pub \
            //tests/perf_test:benchmark_sub \
            //tests/perf_test:cyber_rt_benchmark_suite
```

## Run the benchmark publisher and subscriber

Start a subscriber process in one terminal:

```bash
./bazel-bin/tests/perf_test/benchmark_sub --help
```

Then start a publisher in another terminal:

```bash
./bazel-bin/tests/perf_test/benchmark_pub --help
```

If the benchmark binary accepts arguments for payload size, message count, or concurrency, pass them explicitly to match your workload.

## Run the aggregate benchmark suite

```bash
./bazel-bin/tests/perf_test/cyber_rt_benchmark_suite --help
```

This suite is designed to gather a broader set of runtime measurements and emit machine-readable output for comparison.

## Zero-copy validation guidance

For same-host shared-memory and large-payload tests, validate the following:

- borrowed / zero-copy message counts are non-zero when the transport supports it
- copy counts remain near zero when zero-copy is expected
- message integrity remains correct after transfer
- memory growth stays bounded over repeated runs

This repository’s benchmark design favors bounded execution and relative checks over brittle absolute latency thresholds. Avoid hardcoding strict timing limits unless the benchmark is explicitly designed for a controlled environment.

## Performance test principles

Use these rules when writing or extending performance validation:

- keep test scope explicit and bounded
- prefer relative throughput checks over single-machine absolute time assertions
- validate correctness together with speed
- keep payload sizes representative of actual traffic patterns
- document assumptions about same-host, SHM, or RTPS mode

## Recommended workflow

1. run correctness tests first
2. run the local benchmark target
3. validate zero-copy and POD behavior
4. compare repeated runs to identify regressions
5. treat large outliers as signals for deeper investigation, not as a reason to commit flaky thresholds

## Related docs

- [Zero-copy and sensor data](zero-copy-and-sensor-data.md)
- [Record and replay](record-and-replay.md)
- [Release and verification](release-and-verification.md)
- [Topology and transport](topology-and-transport.md)

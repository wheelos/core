# WheelOS Core release readiness

This checklist maps the release selling points to a reproducible artifact or
validation command. A release is complete only when the commands below pass
on the target hardware and their output is archived with the release.

## Performance

| Selling point | Evidence |
| --- | --- |
| Same-host zero-copy | `bash scripts/release/run_performance_baseline.sh --full`; publish only cases with `zero_copy_copy_count=0`, zero loss/hash mismatch, bounded p99 latency, and archived CPU/RSS. |
| Record data written/read from disk | `bazel-bin/cyber/record/record_io_perf --messages=N --payload_size=...` reports write/read throughput; `bash scripts/record_perf_reader.sh <local-record> 0 artifacts/performance/record-reader-<id>` reports reader modes. Publish write and read as separate metrics, with payload bytes, wall time, I/O counters, chunk size, storage type, and CPU/RSS. |
| Mixed record protobuf/POD benchmark | `bash scripts/record_play_pod_benchmark.sh`; this keeps every source channel and message in order, converts the two camera channels and Velodyne `PointCloud2` channel to `PodMessage`, preserves all other protobuf bytes/type/descriptor, and compares full-protobuf parsing with mixed protobuf/POD reading. Archive the generated `.record`, `.manifest.tsv`, and benchmark output. |

The performance claims are hardware-specific. Publish the CPU, kernel,
transport configuration, payload sizes, duration, and Git SHA alongside the
numbers; do not present the checked-in example numbers as universal
guarantees.

### Canonical sensor benchmark datasets

The regenerated sensor files are test fixtures, not a replacement for the
original record. Protobuf and POD fixtures must be generated from the same
source messages, in the same channel/timestamp/frame order, so every result is
a paired comparison.

The dataset levels are:

| Level | Sampling rule | Purpose |
| --- | --- | --- |
| `smoke` | Four messages per sensor channel | Fast correctness and CI checks |
| `standard` | 64 messages per sensor channel | Release performance baseline |
| `full` | All selected source messages | Long-run throughput and stability |

The selected channels are the two camera channels and the Velodyne64
PointCloud channel used by `record_play_tool`. Camera payloads contain the
protobuf `Image.data` bytes. LiDAR payloads use the POD v1 point layout
`float32 x/y/z`, `uint32 intensity`, and `uint64 timestamp`, for 24 bytes per
point. The layout is little-endian and its field offsets, alignment, and
schema version must remain explicit before fixtures are used across x86 and
ARM.

Each generated dataset must contain paired Protobuf and POD records plus a
manifest and metadata file:

```text
<dataset>.protobuf.record
<dataset>.pod.record
<dataset>.manifest.tsv
<dataset>.metadata.json
```

The manifest records the source and generated SHA256 values, channel,
timestamp, frame ID, original protobuf size, POD payload size, payload hash,
and conversion status. Metadata records the source record, sampling level,
generator/schema versions, conversion parameters, platform, and Git SHA.
Generation fails rather than silently skipping a message, and validation must
confirm equal message counts, channel/timestamp ordering, payload hashes,
LiDAR point counts, POD header sizes, and successful `BorrowFromArray`.

The benchmark matrix is intentionally separated into three measurements:

1. Protobuf record read and full message parse.
2. POD record read and `BorrowFromArray` view construction.
3. Cross-process transport `Writer::Loan`/publish and subscriber borrow,
   including loss, hash mismatches, borrowed/copy counts, latency, CPU, and
   RSS.

The second measurement is not evidence of transport zero-copy. Only the third
measurement validates the end-to-end loan/borrow path. The current `record_play_pod_benchmark.sh` provides the mixed conversion and
first two measurements; transport validation remains a separate release gate.
Use `--mode=mixed --max_per_channel=0` for a complete mixed conversion (the
benchmark script defaults to 64 messages per channel). Conversion fails on a
descriptor, sensor decode, POD serialization, or record-write error; it does
not silently drop messages. The generated output reports per-message-type and
total counts, and the converter preserves channel order, message order, and
timestamps for the selected source messages.

For a publishable record comparison, copy both records to local storage and
run the alternating multi-round baseline:

```bash
bash scripts/release/run_record_pod_performance_baseline.sh \
  --protobuf-record /local/path/sensor_rgb.protobuf.record \
  --mixed-record /local/path/sensor_rgb.mixed_pod.record \
  --outdir artifacts/performance/record-pod-<id> \
  --rounds 5
```

This produces `record_baseline.json`, `summary.md`, per-round logs, and
`metadata.txt`. It reports median total throughput together with record-read
and application-processing time. Network filesystem results are rejected by
default because NFS/cache/writeback behavior is storage data, not a
protobuf-versus-POD performance claim. Use `--allow-network-fs` only for a
separately labelled diagnostic run.

For the zero-copy claim, archive the full `run_performance_baseline.sh`
output separately from the record benchmark. The release gate requires the
POD SHM cases to report `zero_copy_copy_count=0`, positive borrowed-message
counts, zero loss and hash mismatch, and successful completion under the
declared payload sweep. Record borrowing and transport loan/borrow must never
be combined into one speedup number.

### POD Camera application example

Build the two single-purpose examples:

```bash
bazel build //examples/record_play:pod_image_publisher \
  //examples/record_play:pod_image_subscriber
```

Start a subscriber in one process:

```bash
./bazel-bin/examples/record_play/pod_image_subscriber \
  --channel=/example/sensor/camera
```

Publish camera POD frames from another process:

```bash
./bazel-bin/examples/record_play/pod_image_publisher \
  --channel=/example/sensor/camera \
  --bytes=6220800 --count=100
```

The publisher uses `Writer<PodMessage>::Loan`, writes an image
`PodChunkHeader` and payload into the loaned buffer, then calls `Publish`. The
subscriber validates the header and payload size and reports whether the
received message is borrowed. Use an Iceoryx/SHM-capable transport
configuration; a failed loan is an explicit transport capability error, not a
fallback to a copying publish. LiDAR applications use the same two-program
pattern with `PodPayloadKind::POINT_CLOUD`, a 24-byte point layout, and
LiDAR-specific header dimensions.

Record write/read and transport publish/receive are independent performance
claims. Record benchmarks measure storage plus record framing; transport
benchmarks measure delivery, latency, loss, and loan/borrow behavior. Do not
compare their MB/s values directly.

The current local `/tmp` baseline is representative of the two separate
claims:

| Measurement | Result | Interpretation |
| --- | ---: | --- |
| 1 GiB synthetic Record write | 1249.93 MiB/s | Local NVMe write path with record framing |
| 1 GiB synthetic Record read | 2044.87 MiB/s | Local cached/read path with record framing |
| Full sensor protobuf parse | 453.352 MB/s | 43,820-message application baseline |
| Full mixed POD/protobuf processing | 732.378 MB/s | 1.615x total throughput; 65.1x lower application processing time |
| POD 7 MiB transport p99 | 3.716 ms | 210 MiB/s, zero loss, loan/borrow verified |

The synthetic Record result is a storage/framing baseline, while the sensor
result includes application parsing. The transport result is a separate
delivery benchmark; these numbers are not interchangeable.

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

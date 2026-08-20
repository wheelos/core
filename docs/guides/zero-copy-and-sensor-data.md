# Zero-Copy and Sensor Data

This guide covers the advanced zero-copy and high-bandwidth sensor data path used for camera and lidar workloads.

## Use cases

- Camera image transport
- LiDAR point cloud transport
- Large payload processing
- Same-host and low-latency pipeline optimization

## Repository references

- `examples/record_play/`
- `examples/record_play/pod_image_publisher.cc`
- `examples/record_play/pod_image_subscriber.cc`
- `examples/record_play/record_play.h`
- `tests/perf_test/benchmark_pub.cc`
- `tests/perf_test/benchmark_sub.cc`

## Why zero-copy matters

For large sensor messages, copying payloads repeatedly increases memory traffic and latency. Zero-copy techniques reduce unnecessary memory moves and improve throughput.

This repository already includes performance and example code for sensor-oriented payload patterns, especially in the `record_play` and benchmark facilities.

## Camera and LiDAR examples

The `record_play` example uses sensor-oriented payloads and checks channel-based processing for image and point cloud data.

Typical workflow:

1. Publish sensor payloads
2. Subscribe to selected channels
3. Validate payload integrity and timestamps
4. Measure copy-vs-zero-copy behavior

## Performance guidance

- Use zero-copy for same-host transfers when supported
- Keep payload layout compact and explicit
- Validate message integrity after transport
- Measure with realistic sensor data sizes

## Safety and validation

When using high-throughput payloads, verify:

- timestamps are preserved
- messages remain readable after transport
- the zero-copy path does not drop or corrupt data

## Next steps

- [Python quick start](python-quickstart.md)
- [Record and replay](record-and-replay.md)
- [Topology and transport](topology-and-transport.md)

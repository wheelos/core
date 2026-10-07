# Middleware evolution roadmap

## Current stable baseline

- Fast DDS `2.14.6`
- Fast CDR `2.2.7`
- carried Fast CDR string-serialization patch for embedded-`\0` protobuf string compatibility
- durable example and RTPS regression coverage in Bazel/CI

## Recommended stages

1. **Stabilize the 2.14.x line**
   - keep patch surface minimal
   - preserve the current example/RTPS regression suite as a release gate
   - continue validating record, FlatBuffers, and pycyber surfaces together

2. **Strengthen zero-copy on the same host**
   - improve intra-process and shared-memory/data-sharing paths first
   - add coverage for larger payloads, fanout, fanin, and service bursts before changing APIs
   - prefer bounded, discovery-aware regression checks over brittle latency assertions
   - for Pod/raw payloads, use `PodMessage` with `wheelos.cyber.pod_schema/v1`
     record metadata; do not register this schema through `ProtobufFactory`
   - same-host Pod data should use the ICEORYX loan/borrow path; cross-host
     delivery remains RTPS and is covered as a separate serialized copy path

## ICEORYX production operation notes

- Prefer an externally managed RouDi in production:
  - set `CYBER_ICEORYX_START_ROUDI=0`
  - start RouDi under systemd, container orchestration, or another supervisor
  - keep the resource prefix/domain stable across cooperating Cyber processes
- The embedded RouDi path is for local development and tests. It is bound to the
  parent process lifetime with `PR_SET_PDEATHSIG` and should not be treated as
  a high-availability production daemon.
- Large Pod payload mempool sizing is configurable:
  - `CYBER_ICEORYX_ROUDI_LOCK`
  - `CYBER_ICEORYX_MEMPOOL_CHUNK_SIZE`
  - `CYBER_ICEORYX_MEMPOOL_CHUNK_COUNT`
- Default Pod validation currently uses 8 MiB chunks to cover the
  source record camera payloads (~6.2 MiB each).
- Required zero-copy transport regression gates:
  - `bazel test //cyber/transport:transport_test --test_output=errors`
  - `bazel test //cyber/transport/integration_test:hybrid_transceiver_test --test_output=errors`
  - `bazel test //examples/record_play:record_play_tool_test //examples/record_play:record_play_test --test_output=errors`
- Record-play benchmark entrypoint:
  - `bazel build //examples/record_play:record_play_tool_example`
  - `source scripts/env/runtime.bash`
  - `./bazel-bin/examples/record_play/record_play_tool_example --mode=benchmark --source=/mnt/synology/apollo/sensor_rgb.record --output=/tmp/record_play_pod.record --manifest=/tmp/record_play_pod.manifest.tsv --max_per_channel=64`
  - parse the emitted `baseline` line for protobuf/POD messages, bytes, MB/s,
    msg/s, and speedup.
- Known remaining production work:
  - generic `cyber_recorder record -a` still needs a stronger dynamic-discovery
    acceptance test for short-lived Pod writers; conversion and playback paths
    are already Pod-schema-aware
  - add RouDi restart/re-register validation with an externally managed daemon
  - add slow-consumer matrices for every QoS/backpressure mode

3. **Introduce heterogeneous memory abstractions**
   - add explicit buffer-handle negotiation only after CPU shared-memory semantics are stable
   - model the design on ROS 2 type adaptation / negotiation and NITROS-style accelerator handles
   - require cross-device fallback behavior and observability before rollout

4. **Re-evaluate Fast DDS 3.x later**
   - treat 3.x as a migration project, not a routine upgrade
   - only revisit after the current `fastrtps`-style APIs are isolated behind thinner compatibility seams

## Release gates

- `bash scripts/release/ubuntu2204_baseline.sh`
- `bash scripts/release/build_release_artifacts.sh`
- retained example regression coverage for binary payload integrity, fanout, fanin, payload-size stress, and service burst matrices

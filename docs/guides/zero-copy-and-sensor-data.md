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

## Platform synchronization modes

The GPU zero-copy transport has two intentional platform modes. The payload
ownership and `GpuWriter`/`GpuReader` API are shared, but the buffer-sharing
and synchronization backends are not interchangeable.

| Platform | Buffer sharing | Synchronization | Handoff behavior |
|---|---|---|---|
| x86 + discrete GPU | CUDA IPC memory handles | CUDA IPC events recorded on CUDA streams | Asynchronous stream wait; no CPU wait in the normal handoff |
| Orin + integrated GPU | POSIX SHM per slot, `MAP_SHARED`, `cudaHostRegisterMapped`, `cudaHostGetDevicePointer` | Host-synchronized completion fence | `cudaStreamSynchronize` at publish and completion; zero-copy payload, but CPU-blocking handoff |

The control plane exchanges versioned descriptors, slot IDs, metadata,
session IDs, and fences. It never exchanges a raw device pointer. A process
must create its own process-local device pointer after importing and mapping
the shared resource.

### x86 and discrete GPU mode

The writer allocates device memory and exports a
`cudaIpcMemHandle_t`. Each reader imports the handle with
`cudaIpcOpenMemHandle`. Synchronization events are created with
`cudaEventInterprocess`, exported during session setup, and imported by each
peer. The normal sequence is:

1. The writer obtains a loan and waits asynchronously on all previous
   consumer completion events in its writer stream.
2. The writer writes the slot and records a producer CUDA IPC event.
3. The reader inserts a wait for the producer event in its reader stream.
4. The reader runs its CUDA work and records a consumer completion event.
5. The writer reuses the slot only after all imported consumer events report
   completion.

The exporter must keep the allocation and events alive until all importing
processes have stopped using them and the session is closed. Cross-process
tests must use independent `exec` processes; a child created after the parent
has initialized CUDA is not a valid substitute.

### Orin UMA mode

The current Orin path uses one POSIX shared-memory object per slot. Each
process maps the object with `MAP_SHARED`, registers its own mapping with
`cudaHostRegisterMapped`, and obtains a process-local device pointer with
`cudaHostGetDevicePointer`. Device pointers must never be sent to another
process; only the versioned shared-memory descriptor is exchanged.

This path provides zero-copy payload sharing, but synchronization is
intentionally conservative. Before publishing a slot and before acknowledging
consumer completion, the owning process calls `cudaStreamSynchronize`. The
control message then carries a host-synchronized completion fence. This avoids
depending on classic CUDA IPC events, which are not generally available on
Orin driver stacks. It also means zero-copy does not imply CPU-nonblocking
handoff.

The ownership contract is:

1. The writer owns a loaned slot and writes it on its configured CUDA stream.
2. Publish synchronizes that stream before sending the slot descriptor.
3. Each reader consumes the mapped slot on its configured stream.
4. Completion synchronizes the reader stream before sending its ACK.
5. The writer reuses the slot only after every registered reader completes.
6. Timeout or unregister without a completion fence quarantines the slot; it
   is not treated as proof that GPU access stopped.

Run the hardware-backed regression matrix with:

```bash
bazel test --cache_test_results=no \
  //cyber/transport/nvsci:nvsci_buf_pool_test \
  //cyber/transport/nvsci:nvsci_sync_engine_test \
  //cyber/transport/nvsci:gpu_channel_session_test \
  //cyber/transport/nvsci:gpu_writer_reader_test \
  //tests/perf_test:cuda_ipc_process_test \
  //tests/perf_test:gpu_zero_copy_perf_test

bazel test --cache_test_results=no \
  --test_env=CYBER_GPU_IPC_E2E=1 \
  --test_env=CYBER_GPU_FORCE_UMA_SHM=1 \
  //tests/integration_test:gpu_writer_reader_ipc_test
```

`CYBER_GPU_FORCE_UMA_SHM` is a validation switch for exercising the Orin
memory path on a non-integrated CUDA GPU. Orin selects the path automatically.

### Common API and ownership rules

Both modes use the same high-level interfaces:

```cpp
GpuWriterOptions options;
options.stream = writer_cuda_stream;
auto writer = CreateGpuWriter<MetaT>(node, channel_name, options);

GpuReaderOptions reader_options;
reader_options.stream = reader_cuda_stream;
auto reader =
    CreateGpuReader<MetaT>(node, channel_name, reader_options, callback);
```

The CUDA stream is mandatory for the high-level reader and should always be
explicit for the writer. A loan is writable only by its writer until
`Publish` succeeds. A received view is read-only until its callback completes
and `Done`/RAII completion has sent an ACK. The writer must not reuse a slot
until every consumer registered for that publication has completed.

Timeout, unregister, or process disappearance is not itself a completion
proof. The affected slot enters quarantine and remains unavailable until a
valid completion fence is observed. Applications should size the pool for
the expected number of concurrent consumers and use an explicit session
restart policy for a consumer that is permanently dead.

## Next steps

- [Python quick start](python-quickstart.md)
- [Record and replay](record-and-replay.md)
- [Topology and transport](topology-and-transport.md)

# GPU zero-copy usage guide

This example shows how to transport an image buffer in GPU memory without
copying the image payload through a CPU message buffer. It uses the
`camera/front` channel:

- `gpu_talker` owns the producer slot pool and publishes 1920 x 1080 RGB8
  frames at 10 Hz.
- `gpu_listener` receives the same GPU buffer, validates the first byte on its
  CUDA stream, and completes the slot with `Done()`.
- The control plane transfers slot descriptors, metadata, and synchronization
  fences; it does not transfer a raw device pointer between processes.

## Prerequisites

Use a host with a supported CUDA runtime and an available CUDA device. Install
the repository build dependencies first:

```bash
sudo bash scripts/deploy/build.sh
```

The source build also requires access to the Bazel registries configured by
this repository. For platform-specific buffer and synchronization behavior,
see [Zero-Copy and Sensor Data](zero-copy-and-sensor-data.md).

## Build the examples

From the repository root:

```bash
bazel build //examples:gpu_talker //examples:gpu_listener
```

Before running binaries from `bazel-bin`, source the runtime environment:

```bash
source scripts/env/runtime.bash
```

## Start the listener and talker

Start the listener first so that it can register before frames are published.
Use two terminals, and source the runtime environment in each terminal.

**Terminal 1 — listener**

```bash
source scripts/env/runtime.bash
./bazel-bin/examples/gpu_listener
```

**Terminal 2 — talker**

```bash
source scripts/env/runtime.bash
./bazel-bin/examples/gpu_talker
```

The talker logs a publication message every 100 frames. The listener logs
received frame metadata every 100 frames and validates that the consumer sees
the producer's frame pattern. Press `Ctrl+C` in each terminal to stop the
processes; stop the talker before the listener when possible.

## Application usage

Create a CUDA stream for each producer and consumer, and pass the streams to
the GPU transport options. The writer owns and fills a writable loan. The
reader receives a read-only view and performs its GPU work in the callback:

```cpp
GpuWriterOptions writer_options;
writer_options.slot_count = 4;
writer_options.slot_size = image_bytes;
writer_options.stream = producer_stream;
writer_options.backpressure = GpuBackpressurePolicy::DROP;

auto writer =
    CreateGpuWriter<ImageMeta>(writer_node, "camera/front", writer_options);

GpuReaderOptions reader_options;
reader_options.stream = consumer_stream;

auto reader = CreateGpuReader<ImageMeta>(
    reader_node, "camera/front", reader_options,
    [&](GpuMsgView<ImageMeta>& view) {
      // The transport has inserted the producer wait on consumer_stream.
      SubmitGpuWork(view.device_ptr(), view.capacity(), consumer_stream);
      view.Done(consumer_stream);
    });
```

The actual `SubmitGpuWork` function should use the received device pointer
directly for inference, preprocessing, encoding, or another GPU operation.
Do not copy the payload to a CPU buffer unless the application explicitly
needs a CPU representation.

For the complete ownership sequence, the example demonstrates:

1. The talker borrows a slot with `GpuWriter::Loan()`.
2. It writes the image pattern directly to the loaned GPU buffer with
   `cudaMemsetAsync`.
3. `GpuWriter::Publish()` records producer synchronization and publishes only
   the slot ID, metadata, and fence information.
4. The listener inserts the producer wait on its configured CUDA stream before
   accessing the buffer.
5. The listener calls `Done(infer_stream)` after its GPU work has been queued.
   The writer can reuse the slot only after the consumer completion fence is
   received.

The example uses drop-on-backpressure with a four-slot pool. If all slots are
in use, the talker drops that frame rather than blocking the capture loop.
Applications can select `BLOCK` or `TIMEOUT` instead when dropping is not
acceptable.

## Important precautions

- **Use explicit CUDA streams.** A GPU reader requires a non-null stream. Use
  the same stream for consumer work and `Done()` so the completion fence is
  ordered after the last GPU operation.
- **Complete every view.** Call `Done(stream)` after all GPU work using the
  buffer has been queued. The view's RAII cleanup is a safety net, not a
  replacement for explicit completion in asynchronous pipelines.
- **Treat received memory as read-only.** `GpuMsgView::device_ptr()` is a
  `const void*`. Do not modify a received buffer in place, especially when
  the channel has multiple consumers.
- **Keep work within the slot capacity.** Validate
  `width * height * channels` or the application payload size against
  `view.capacity()` before launching GPU work.
- **Do not retain the pointer or view.** Both are valid only until the
  callback completes and the view is done. If work is submitted
  asynchronously, it must be ordered on the configured stream before
  `Done()` returns.
- **Do not exchange raw device pointers between processes.** The transport
  exchanges buffer descriptors, slot IDs, metadata, and synchronization
  fences. Each process obtains its own process-local device pointer.
- **Size the pool for in-flight work.** With `DROP`, a full pool drops frames.
  A slow or disconnected consumer can keep slots unavailable until timeout
  handling quarantines them.
- **Shut down in order.** Stop publishing, shut down the reader/writer, wait
  for the configured CUDA streams, and only then destroy the streams.
- **Account for platform behavior.** Discrete GPU mode uses CUDA IPC for
  memory and synchronization. Orin UMA mode uses mapped POSIX shared memory
  and conservative host synchronization, so zero-copy does not guarantee a
  CPU-nonblocking handoff.

## Next steps

- [General Quick Start](../getting-started/quickstart.md)
- [Zero-Copy and Sensor Data](zero-copy-and-sensor-data.md)
- [Source build and run](source-build-and-run.md)

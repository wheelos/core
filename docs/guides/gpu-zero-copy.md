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

The default Bazel configuration is CPU-only and does not require CUDA headers
or libraries. GPU targets are enabled explicitly on a CUDA host. Install the
repository build dependencies first:

```bash
sudo bash scripts/deploy/build.sh
```

The source build also requires access to the Bazel registries configured by
this repository. For platform-specific buffer and synchronization behavior,
see [Zero-Copy and Sensor Data](zero-copy-and-sensor-data.md).

## Build the examples

From the repository root, use `--config=cuda` for x86 with CUDA IPC. It enables
the repository's `rules_cuda` toolchain and expects the CUDA toolkit at
`/usr/local/cuda` (the path configured in `MODULE.bazel`):

```bash
bazel build --config=cuda \
  //examples:gpu_talker \
  //examples:gpu_listener \
  //examples:gpu_inference_talker \
  //examples:gpu_inference_listener
```

On a target configured for NvSci, use `--config=orin` instead; the target
NvSci SDK libraries must be installed. CUDA-only GPU targets are marked
incompatible in the default CPU build, so commands such as `bazel build
//cyber/...` do not try to compile CUDA examples or tests.

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

## Image-to-inference example

`gpu_inference_talker` and `gpu_inference_listener` form a finite publisher and
a long-running consumer on `camera/inference`. The publisher accepts a binary
P6 PPM image with 8-bit RGB channels, uploads it to a pinned host staging
buffer, then copies it into each loaned GPU slot. After publication, the
listener runs a small two-class linear classifier directly on
`GpuMsgView::device_ptr()`; it copies only the small inference result back to
the CPU. This is a lightweight CUDA-kernel example, not a PyTorch model.

Create a small red test image:

```bash
python3 - <<'PY'
with open("/tmp/red.ppm", "wb") as image:
    image.write(b"P6\n224 224\n255\n")
    image.write(bytes((255, 0, 0)) * (224 * 224))
PY
```

Start the listener first, then publish 20 frames:

```bash
# Terminal 1
source scripts/env/runtime.bash
./bazel-bin/examples/gpu_inference_listener 1

# Terminal 2
source scripts/env/runtime.bash
./bazel-bin/examples/gpu_inference_talker /tmp/red.ppm 20
```

The two processes must overlap during startup: the listener bootstraps its GPU
session from the publisher, and the publisher waits for listener registration.
Both wait up to 30 seconds, so launch the publisher soon after starting the
listener rather than waiting for the listener's ready log first.
The example requires a registered consumer for each publish and waits for an
ACK of the first frame before sending the rest. That bounded data-plane
handshake detects a listener that is registered but not yet receiving data,
instead of relying on a fixed discovery delay.
With the runtime environment above, GLOG output is written under `data/log`.

Class `1` corresponds to red-dominant input; class `0` corresponds to dark
input. The listener reports the class, channel means, GPU kernel time, and
end-to-end age. Press `Ctrl+C` in the listener after the sender exits. The
sender waits for a registered consumer and for all published slots to be
returned before exiting.

The image file is uploaded once per frame on the producer. That H2D operation
is input acquisition, not a transport copy. On the consumer, the full image
is never copied: the classifier receives the imported slot pointer directly,
and only its compact result is copied to host memory.

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
- **Keep producer writes on the loan's stream.** Dropping an unpublished loan
  records a producer fence before its slot becomes reusable; GPU work submitted
  on other streams must be synchronized by the application first.
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

// Copyright 2026 WheelOS All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <memory>

#include <cuda_runtime.h>

#include "examples/gpu_image_meta.h"

#include "cyber/common/log.h"
#include "cyber/cyber.h"
#include "cyber/time/rate.h"
#include "cyber/time/time.h"
#include "cyber/transport/nvsci/gpu_writer.h"

using apollo::cyber::Rate;
using apollo::cyber::Time;
using apollo::cyber::examples::GpuImageMeta;
using apollo::cyber::transport::CreateGpuWriter;
using apollo::cyber::transport::GpuBackpressurePolicy;
using apollo::cyber::transport::GpuWriterOptions;

namespace {

constexpr uint32_t kImageWidth = 1920;
constexpr uint32_t kImageHeight = 1080;
constexpr uint32_t kImageChannels = 3;

// Tutorial contract: tightly packed RGB8 image.
constexpr size_t kImageBytes =
    static_cast<size_t>(kImageWidth) *
    static_cast<size_t>(kImageHeight) *
    static_cast<size_t>(kImageChannels);

bool CheckCuda(cudaError_t error, const char* operation) {
  if (error == cudaSuccess) {
    return true;
  }

  AERROR << operation << " failed: " << cudaGetErrorString(error);
  return false;
}

bool GenerateGpuFrame(void* device_ptr, uint8_t value,
                      cudaStream_t stream) {
  return CheckCuda(cudaMemsetAsync(device_ptr, value, kImageBytes, stream),
                   "cudaMemsetAsync");
}

}  // namespace

int main(int argc, char* argv[]) {
  apollo::cyber::Init(argv[0]);

  auto talker_node = apollo::cyber::CreateNode("gpu_talker");

  // 1. Create the CUDA stream used for asynchronous frame generation.
  cudaStream_t capture_stream = nullptr;
  if (!CheckCuda(
          cudaStreamCreateWithFlags(
              &capture_stream, cudaStreamNonBlocking),
          "cudaStreamCreateWithFlags")) {
    return -1;
  }

  // 2. Configure the GPU buffer pool and producer stream.
  GpuWriterOptions options;
  options.slot_count = 4;
  options.slot_size = kImageBytes;
  options.alignment = 4096;
  options.stream = capture_stream;

  // Real-time sensor pipelines generally prefer dropping a frame over
  // blocking the producer when all slots are temporarily in use.
  options.backpressure = GpuBackpressurePolicy::DROP;

  // 3. Create the typed GPU zero-copy writer.
  auto gpu_writer =
      CreateGpuWriter<GpuImageMeta>(
          talker_node, "camera/front", options);

  if (!gpu_writer) {
    AERROR << "Failed to create GpuWriter";
    cudaStreamDestroy(capture_stream);
    return -1;
  }

  Rate rate(10.0);
  uint32_t frame_seq = 0;

  AINFO << "GPU Talker initialized.";
  AINFO << "Publishing " << kImageWidth << "x" << kImageHeight
        << " RGB8 frames at 10 Hz.";

  while (apollo::cyber::OK()) {
    // 4. Borrow one slot from the GPU buffer pool.
    //
    // The returned loan owns the slot until it is published or released
    // by RAII on scope exit.
    auto loan = gpu_writer->Loan();

    if (!loan) {
      AWARN << "GPU buffer pool is full; dropping frame " << frame_seq;
      rate.Sleep();
      continue;
    }

    // 5. Generate the frame directly in GPU memory.
    //
    // No intermediate CPU image buffer is used.
    const uint8_t test_pattern =
        static_cast<uint8_t>(frame_seq & 0xFF);

    if (!GenerateGpuFrame(
            loan->device_ptr(), test_pattern, capture_stream)) {
      AWARN << "Failed to generate frame " << frame_seq;
      rate.Sleep();
      continue;
    }

    // 6. Fill application metadata.
    //
    // The tutorial uses a tightly packed RGB8 image contract:
    //   width    = 1920
    //   height   = 1080
    //   channels = 3
    loan->metadata().width = kImageWidth;
    loan->metadata().height = kImageHeight;
    loan->metadata().channels = kImageChannels;
    loan->metadata().frame_id = frame_seq;
    loan->metadata().timestamp_ns =
        Time::Now().ToNanosecond();

    // 7. Publish the loan.
    //
    // Publish transfers ownership of the slot from the producer-side
    // loan to the transport/consumer lifecycle.
    //
    // The transport layer is responsible for the producer->consumer
    // synchronization required by the GPU transport.
    if (!gpu_writer->Publish(std::move(*loan))) {
      AWARN << "Failed to publish frame " << frame_seq;
    } else if ((frame_seq % 100) == 0) {
      AINFO << "Published frame " << frame_seq;
    }

    ++frame_seq;
    rate.Sleep();
  }

  // Ensure producer-side CUDA work has completed before destroying
  // the stream and shutting down the process.
  if (!CheckCuda(
          cudaStreamSynchronize(capture_stream),
          "cudaStreamSynchronize(capture_stream)")) {
    AERROR << "Producer CUDA stream did not synchronize cleanly";
  }

  gpu_writer->Shutdown();

  if (!CheckCuda(
          cudaStreamDestroy(capture_stream),
          "cudaStreamDestroy(capture_stream)")) {
    return -1;
  }

  return 0;
}

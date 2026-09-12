/******************************************************************************
 * Copyright 2026 WheelOS. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

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

int main(int argc, char* argv[]) {
  apollo::cyber::Init(argv[0]);

  auto talker_node = apollo::cyber::CreateNode("gpu_talker");

  // 1. Initialize CUDA stream for GPU data generation
  cudaStream_t capture_stream = nullptr;
  if (cudaStreamCreateWithFlags(&capture_stream, cudaStreamNonBlocking) !=
      cudaSuccess) {
    AERROR << "Failed to create capture CUDA stream";
    return -1;
  }

  // 2. Configure GPU zero-copy writer options
  constexpr size_t kImageBytes = 1920 * 1080 * 3;
  GpuWriterOptions options;
  options.slot_count = 4;
  options.slot_size = kImageBytes;
  options.alignment = 4096;
  options.stream = capture_stream;
  options.backpressure = GpuBackpressurePolicy::DROP;

  // 3. Create high-level typed GPU zero-copy Writer via Cyber RT
  auto gpu_writer =
      CreateGpuWriter<GpuImageMeta>(talker_node, "camera/front", options);
  if (!gpu_writer) {
    AERROR << "Failed to create GpuWriter";
    cudaStreamDestroy(capture_stream);
    return -1;
  }

  Rate rate(10.0);  // 10 Hz
  uint32_t frame_seq = 0;

  AINFO << "GPU Talker initialized. Starting zero-copy publishing loop...";

  while (apollo::cyber::OK()) {
    // 4. Borrow slot from GPU buffer pool via clean RAII loan
    auto loan = gpu_writer->Loan();
    if (!loan) {
      AWARN << "GPU Buffer pool full, skipping frame " << frame_seq;
      rate.Sleep();
      continue;
    }

    // 5. Asynchronously write data directly on GPU stream (Zero CPU copy)
    const uint8_t test_pattern = static_cast<uint8_t>(frame_seq & 0xFF);
    if (cudaMemsetAsync(loan->device_ptr(), test_pattern, 1024,
                        capture_stream) != cudaSuccess) {
      AWARN << "cudaMemsetAsync failed for frame " << frame_seq;
      rate.Sleep();
      continue;
    }

    // 6. Fill metadata
    loan->metadata().width = 1920;
    loan->metadata().height = 1080;
    loan->metadata().channels = 3;
    loan->metadata().frame_id = frame_seq;
    loan->metadata().timestamp_ns = Time::Now().ToNanosecond();

    // 7. Publish to Cyber RT network (Fences and session tracking handled internally)
    if (!gpu_writer->Publish(std::move(*loan))) {
      AWARN << "Failed to publish GPU frame " << frame_seq;
    } else {
      AINFO << "GPU Talker published frame " << frame_seq;
    }

    frame_seq++;
    rate.Sleep();
  }

  gpu_writer->Shutdown();
  cudaStreamDestroy(capture_stream);
  return 0;
}

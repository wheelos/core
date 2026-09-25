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

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#include <cuda_runtime.h>

#include "examples/gpu_classifier_kernel.h"
#include "examples/gpu_image_meta.h"

#include "cyber/common/log.h"
#include "cyber/cyber.h"
#include "cyber/time/time.h"
#include "cyber/transport/nvsci/gpu_reader.h"

namespace {

using apollo::cyber::examples::GpuClassifierResult;
using apollo::cyber::examples::GpuImageMeta;
using apollo::cyber::transport::GpuMsgView;
using apollo::cyber::transport::GpuReaderOptions;

constexpr char kChannel[] = "camera/inference";
constexpr uint32_t kImageChannels = 3;

bool CheckCuda(cudaError_t error, const char* operation) {
  if (error == cudaSuccess) {
    return true;
  }
  AERROR << operation << " failed: " << cudaGetErrorString(error);
  return false;
}

bool ParseExpectedClass(const char* text, int* expected_class) {
  const std::string value(text);
  int parsed = -1;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      parsed < 0 || parsed > 1) {
    return false;
  }
  *expected_class = parsed;
  return true;
}

bool GetImageSize(const GpuImageMeta& meta, size_t* pixel_count,
                  size_t* byte_count) {
  if (meta.width == 0 || meta.height == 0 || meta.channels != kImageChannels) {
    return false;
  }
  const size_t width = meta.width;
  const size_t height = meta.height;
  if (width > std::numeric_limits<size_t>::max() / height ||
      width * height > std::numeric_limits<size_t>::max() / kImageChannels) {
    return false;
  }
  *pixel_count = width * height;
  *byte_count = *pixel_count * kImageChannels;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc > 2) {
    AERROR << "Usage: gpu_inference_listener [expected_class:0|1]";
    return 2;
  }

  int expected_class = -1;
  if (argc == 2 && !ParseExpectedClass(argv[1], &expected_class)) {
    AERROR << "expected_class must be 0 or 1";
    return 2;
  }

  if (!apollo::cyber::Init(argv[0])) {
    return 1;
  }
  auto node = apollo::cyber::CreateNode("gpu_inference_listener");

  cudaStream_t inference_stream = nullptr;
  if (!CheckCuda(
          cudaStreamCreateWithFlags(&inference_stream, cudaStreamNonBlocking),
          "cudaStreamCreateWithFlags")) {
    return 1;
  }

  GpuClassifierResult* device_result = nullptr;
  GpuClassifierResult* host_result = nullptr;
  cudaEvent_t inference_start = nullptr;
  cudaEvent_t inference_stop = nullptr;
  if (!CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device_result),
                            sizeof(GpuClassifierResult)),
                 "cudaMalloc(result)") ||
      !CheckCuda(
          cudaHostAlloc(reinterpret_cast<void**>(&host_result),
                        sizeof(GpuClassifierResult), cudaHostAllocPortable),
          "cudaHostAlloc(result)") ||
      !CheckCuda(cudaEventCreate(&inference_start), "cudaEventCreate(start)") ||
      !CheckCuda(cudaEventCreate(&inference_stop), "cudaEventCreate(stop)")) {
    if (inference_stop != nullptr) {
      cudaEventDestroy(inference_stop);
    }
    if (inference_start != nullptr) {
      cudaEventDestroy(inference_start);
    }
    if (host_result != nullptr) {
      cudaFreeHost(host_result);
    }
    if (device_result != nullptr) {
      cudaFree(device_result);
    }
    cudaStreamDestroy(inference_stream);
    return 1;
  }

  std::atomic<uint64_t> received_count{0};
  std::atomic<uint64_t> error_count{0};
  std::mutex inference_mutex;
  uint64_t next_frame_id = 0;

  GpuReaderOptions options;
  options.stream = inference_stream;
  options.bootstrap_timeout_ms = 30000;
  auto reader = apollo::cyber::transport::CreateGpuReader<GpuImageMeta>(
      node, kChannel, options, [&](GpuMsgView<GpuImageMeta>& view) {
        std::lock_guard<std::mutex> lock(inference_mutex);
        if (view->frame_id != next_frame_id) {
          AERROR << "Unexpected frame sequence: received " << view->frame_id
                 << ", expected " << next_frame_id;
          error_count.fetch_add(1, std::memory_order_relaxed);
        }
        next_frame_id = view->frame_id + 1;
        auto complete = [&]() {
          if (!view.Done(inference_stream)) {
            AERROR << "Failed to complete GPU frame " << view->frame_id;
            error_count.fetch_add(1, std::memory_order_relaxed);
          }
        };

        size_t pixel_count = 0;
        size_t byte_count = 0;
        if (!GetImageSize(view.metadata(), &pixel_count, &byte_count) ||
            byte_count > view.capacity()) {
          AERROR << "Invalid image dimensions or payload capacity for frame "
                 << view->frame_id;
          error_count.fetch_add(1, std::memory_order_relaxed);
          complete();
          return;
        }

        if (!CheckCuda(cudaEventRecord(inference_start, inference_stream),
                       "cudaEventRecord(start)")) {
          error_count.fetch_add(1, std::memory_order_relaxed);
          complete();
          return;
        }
        const auto* device_rgb = static_cast<const uint8_t*>(view.device_ptr());
        const cudaError_t launch_error =
            apollo::cyber::examples::LaunchGpuClassifier(
                device_rgb, pixel_count, device_result, inference_stream);
        if (!CheckCuda(launch_error, "LaunchGpuClassifier") ||
            !CheckCuda(cudaEventRecord(inference_stop, inference_stream),
                       "cudaEventRecord(stop)") ||
            !CheckCuda(
                cudaMemcpyAsync(host_result, device_result,
                                sizeof(GpuClassifierResult),
                                cudaMemcpyDeviceToHost, inference_stream),
                "cudaMemcpyAsync(inference result)") ||
            !CheckCuda(cudaStreamSynchronize(inference_stream),
                       "cudaStreamSynchronize(inference)")) {
          error_count.fetch_add(1, std::memory_order_relaxed);
          complete();
          return;
        }

        float inference_ms = 0.0f;
        const bool timing_ok =
            CheckCuda(cudaEventElapsedTime(&inference_ms, inference_start,
                                           inference_stop),
                      "cudaEventElapsedTime");
        const uint64_t now_ns = apollo::cyber::Time::Now().ToNanosecond();
        const uint64_t latency_us = now_ns >= view->timestamp_ns
                                        ? (now_ns - view->timestamp_ns) / 1000
                                        : 0;
        const bool result_ok =
            timing_ok && host_result->class_id <= 1 &&
            (expected_class < 0 ||
             host_result->class_id == static_cast<uint32_t>(expected_class));
        if (!result_ok) {
          AERROR << "Inference validation failed for frame " << view->frame_id
                 << ": class=" << host_result->class_id
                 << ", expected=" << expected_class;
          error_count.fetch_add(1, std::memory_order_relaxed);
        } else {
          AINFO << "Inference frame=" << view->frame_id
                << " class=" << host_result->class_id << " mean_rgb=["
                << host_result->mean_rgb[0] << ", " << host_result->mean_rgb[1]
                << ", " << host_result->mean_rgb[2] << "]"
                << " logits=[" << host_result->logits[0] << ", "
                << host_result->logits[1] << "]"
                << " gpu_inference_ms=" << inference_ms
                << " end_to_end_us=" << latency_us;
        }
        received_count.fetch_add(1, std::memory_order_relaxed);
        complete();
      });

  if (!reader) {
    AERROR << "Could not create GPU reader";
    cudaEventDestroy(inference_stop);
    cudaEventDestroy(inference_start);
    cudaFreeHost(host_result);
    cudaFree(device_result);
    cudaStreamDestroy(inference_stream);
    return 1;
  }

  AINFO << "GPU inference listener ready on " << kChannel
        << "; waiting for RGB8 PPM frames";
  apollo::cyber::WaitForShutdown();
  reader->Shutdown();
  const bool stream_ok = CheckCuda(cudaStreamSynchronize(inference_stream),
                                   "cudaStreamSynchronize(inference_stream)");
  AINFO << "Received " << received_count.load(std::memory_order_relaxed)
        << " frames; inference errors="
        << error_count.load(std::memory_order_relaxed);

  const bool cleanup_ok =
      CheckCuda(cudaEventDestroy(inference_stop), "cudaEventDestroy(stop)") &&
      CheckCuda(cudaEventDestroy(inference_start), "cudaEventDestroy(start)") &&
      CheckCuda(cudaFreeHost(host_result), "cudaFreeHost(result)") &&
      CheckCuda(cudaFree(device_result), "cudaFree(result)") &&
      CheckCuda(cudaStreamDestroy(inference_stream),
                "cudaStreamDestroy(inference_stream)");
  return stream_ok && cleanup_ok && error_count.load() == 0 ? 0 : 1;
}

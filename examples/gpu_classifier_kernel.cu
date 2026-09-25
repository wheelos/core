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

#include "gpu_classifier_kernel.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace {

constexpr uint32_t kReductionThreads = 256;

__global__ void ComputeRgbMean(const uint8_t* rgb, size_t pixel_count,
                               GpuClassifierResult* result) {
  __shared__ unsigned long long sums[3][kReductionThreads];
  const uint32_t thread_id = threadIdx.x;
  unsigned long long red = 0;
  unsigned long long green = 0;
  unsigned long long blue = 0;

  for (size_t pixel = thread_id; pixel < pixel_count; pixel += blockDim.x) {
    const size_t offset = pixel * 3;
    red += rgb[offset];
    green += rgb[offset + 1];
    blue += rgb[offset + 2];
  }

  sums[0][thread_id] = red;
  sums[1][thread_id] = green;
  sums[2][thread_id] = blue;
  __syncthreads();

  for (uint32_t stride = kReductionThreads / 2; stride != 0; stride /= 2) {
    if (thread_id < stride) {
      sums[0][thread_id] += sums[0][thread_id + stride];
      sums[1][thread_id] += sums[1][thread_id + stride];
      sums[2][thread_id] += sums[2][thread_id + stride];
    }
    __syncthreads();
  }

  if (thread_id == 0) {
    const float denominator = static_cast<float>(pixel_count) * 255.0f;
    result->mean_rgb[0] = static_cast<float>(sums[0][0]) / denominator;
    result->mean_rgb[1] = static_cast<float>(sums[1][0]) / denominator;
    result->mean_rgb[2] = static_cast<float>(sums[2][0]) / denominator;
  }
}

__global__ void RunLinearClassifier(GpuClassifierResult* result) {
  const float brightness =
      (result->mean_rgb[0] + result->mean_rgb[1] + result->mean_rgb[2]) / 3.0f;
  result->logits[0] = 0.5f - brightness;
  result->logits[1] = result->mean_rgb[0] - result->mean_rgb[2] - 0.1f;
  result->class_id = result->logits[1] > result->logits[0] ? 1U : 0U;
}

}  // namespace

cudaError_t LaunchGpuClassifier(const uint8_t* device_rgb, size_t pixel_count,
                                GpuClassifierResult* device_result,
                                cudaStream_t stream) {
  if (device_rgb == nullptr || pixel_count == 0 || device_result == nullptr ||
      stream == nullptr) {
    return cudaErrorInvalidValue;
  }

  ComputeRgbMean<<<1, kReductionThreads, 0, stream>>>(device_rgb, pixel_count,
                                                      device_result);
  cudaError_t error = cudaGetLastError();
  if (error != cudaSuccess) {
    return error;
  }

  RunLinearClassifier<<<1, 1, 0, stream>>>(device_result);
  return cudaGetLastError();
}

}  // namespace examples
}  // namespace cyber
}  // namespace apollo

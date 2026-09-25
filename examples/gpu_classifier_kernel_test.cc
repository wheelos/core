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

#include "examples/gpu_classifier_kernel.h"

#include <array>
#include <cstdint>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

namespace apollo {
namespace cyber {
namespace examples {
namespace {

TEST(GpuClassifierKernelTest, ClassifiesBlackAndRedImagesOnDevice) {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    GTEST_SKIP() << "A CUDA device is required for the kernel test.";
  }

  constexpr size_t kPixelCount = 4;
  constexpr size_t kByteCount = kPixelCount * 3;
  const std::array<uint8_t, kByteCount> black = {};
  const std::array<uint8_t, kByteCount> red = {255, 0, 0, 255, 0, 0,
                                               255, 0, 0, 255, 0, 0};

  uint8_t* device_rgb = nullptr;
  GpuClassifierResult* device_result = nullptr;
  cudaStream_t stream = nullptr;
  ASSERT_EQ(cudaMalloc(reinterpret_cast<void**>(&device_rgb), kByteCount),
            cudaSuccess);
  ASSERT_EQ(cudaMalloc(reinterpret_cast<void**>(&device_result),
                       sizeof(GpuClassifierResult)),
            cudaSuccess);
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
            cudaSuccess);

  GpuClassifierResult result{};
  ASSERT_EQ(cudaMemcpyAsync(device_rgb, black.data(), kByteCount,
                            cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(LaunchGpuClassifier(device_rgb, kPixelCount, device_result, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(&result, device_result, sizeof(result),
                            cudaMemcpyDeviceToHost, stream),
            cudaSuccess);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);
  EXPECT_EQ(result.class_id, 0U);
  EXPECT_FLOAT_EQ(result.mean_rgb[0], 0.0f);
  EXPECT_FLOAT_EQ(result.mean_rgb[1], 0.0f);
  EXPECT_FLOAT_EQ(result.mean_rgb[2], 0.0f);

  ASSERT_EQ(cudaMemcpyAsync(device_rgb, red.data(), kByteCount,
                            cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(LaunchGpuClassifier(device_rgb, kPixelCount, device_result, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(&result, device_result, sizeof(result),
                            cudaMemcpyDeviceToHost, stream),
            cudaSuccess);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);
  EXPECT_EQ(result.class_id, 1U);
  EXPECT_FLOAT_EQ(result.mean_rgb[0], 1.0f);
  EXPECT_FLOAT_EQ(result.mean_rgb[1], 0.0f);
  EXPECT_FLOAT_EQ(result.mean_rgb[2], 0.0f);

  EXPECT_EQ(cudaStreamDestroy(stream), cudaSuccess);
  EXPECT_EQ(cudaFree(device_result), cudaSuccess);
  EXPECT_EQ(cudaFree(device_rgb), cudaSuccess);
}

}  // namespace
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

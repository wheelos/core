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

#ifndef EXAMPLES_GPU_CLASSIFIER_KERNEL_H_
#define EXAMPLES_GPU_CLASSIFIER_KERNEL_H_

#include <cstddef>
#include <cstdint>

#include <cuda_runtime.h>

namespace apollo {
namespace cyber {
namespace examples {

struct GpuClassifierResult {
  float mean_rgb[3] = {};
  float logits[2] = {};
  uint32_t class_id = 0;
};

cudaError_t LaunchGpuClassifier(const uint8_t* device_rgb, size_t pixel_count,
                                GpuClassifierResult* device_result,
                                cudaStream_t stream);

}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // EXAMPLES_GPU_CLASSIFIER_KERNEL_H_

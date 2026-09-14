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

#ifndef EXAMPLES_GPU_IMAGE_META_H_
#define EXAMPLES_GPU_IMAGE_META_H_

#include <cstdint>

namespace apollo {
namespace cyber {
namespace examples {

struct alignas(8) GpuImageMeta {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t channels = 3;
  uint32_t frame_id = 0;
  uint64_t timestamp_ns = 0;
};

}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // EXAMPLES_GPU_IMAGE_META_H_

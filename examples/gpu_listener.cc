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
#include "cyber/transport/nvsci/gpu_reader.h"

using apollo::cyber::examples::GpuImageMeta;
using apollo::cyber::transport::CreateGpuReader;
using apollo::cyber::transport::GpuMsgView;
using apollo::cyber::transport::GpuReaderOptions;

int main(int argc, char* argv[]) {
  apollo::cyber::Init(argv[0]);

  auto listener_node = apollo::cyber::CreateNode("gpu_listener");

  // 1. Initialize CUDA stream for GPU consumer kernel
  cudaStream_t infer_stream = nullptr;
  if (cudaStreamCreateWithFlags(&infer_stream, cudaStreamNonBlocking) !=
      cudaSuccess) {
    AERROR << "Failed to create inference CUDA stream";
    return -1;
  }

  // 2. Configure reader options
  GpuReaderOptions options;
  options.stream = infer_stream;

  // 3. Create high-level typed GPU zero-copy Reader via Cyber RT
  auto gpu_reader = CreateGpuReader<GpuImageMeta>(
      listener_node, "camera/front", options,
      [&](const GpuMsgView<GpuImageMeta>& view) {
        // Asynchronous hardware wait has already been mounted on infer_stream!
        AINFO << "GPU Listener processing frame " << view->frame_id
              << " [" << view->width << "x" << view->height << "]"
              << " on GPU ptr: " << view.device_ptr();

        // Downstream GPU processing (e.g. inference, filter, encoding) runs directly
        // on view.device_ptr() on infer_stream...

        // View destructor automatically generates postfence on infer_stream
        // and sends reverse completion ACK back to writer!
      });

  if (!gpu_reader) {
    AERROR << "Failed to create GpuReader";
    cudaStreamDestroy(infer_stream);
    return -1;
  }

  AINFO << "GPU Listener initialized. Ready to receive zero-copy GPU frames.";

  apollo::cyber::WaitForShutdown();

  cudaStreamDestroy(infer_stream);
  return 0;
}

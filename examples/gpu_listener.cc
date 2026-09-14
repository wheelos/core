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
#include "cyber/transport/nvsci/gpu_reader.h"

using apollo::cyber::examples::GpuImageMeta;
using apollo::cyber::transport::CreateGpuReader;
using apollo::cyber::transport::GpuMsgView;
using apollo::cyber::transport::GpuReaderOptions;

namespace {

bool CheckCuda(cudaError_t error, const char* operation) {
  if (error == cudaSuccess) {
    return true;
  }

  AERROR << operation << " failed: " << cudaGetErrorString(error);
  return false;
}

bool ValidateGpuImage(const void* device_ptr, size_t byte_size,
                      uint8_t expected_value, cudaStream_t stream) {
  if (byte_size == 0) {
    return false;
  }

  uint8_t first_byte = 0;
  if (!CheckCuda(cudaMemcpyAsync(&first_byte, device_ptr, sizeof(first_byte),
                                 cudaMemcpyDeviceToHost, stream),
                 "cudaMemcpyAsync")) {
    return false;
  }
  if (!CheckCuda(cudaStreamSynchronize(stream),
                 "cudaStreamSynchronize(image validation)")) {
    return false;
  }
  if (first_byte != expected_value) {
    AERROR << "Received unexpected first pixel byte: "
           << static_cast<unsigned int>(first_byte) << ", expected "
           << static_cast<unsigned int>(expected_value);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  apollo::cyber::Init(argv[0]);

  auto listener_node =
      apollo::cyber::CreateNode("gpu_listener");

  // 1. Create the CUDA stream used by the consumer GPU pipeline.
  cudaStream_t infer_stream = nullptr;
  if (!CheckCuda(
          cudaStreamCreateWithFlags(
              &infer_stream, cudaStreamNonBlocking),
          "cudaStreamCreateWithFlags")) {
    return -1;
  }

  // 2. Bind the reader to the consumer CUDA stream.
  //
  // The GPU transport is responsible for arranging the producer->consumer
  // synchronization on this stream before downstream work is submitted.
  GpuReaderOptions options;
  options.stream = infer_stream;

  // 3. Create the typed GPU zero-copy reader.
  auto gpu_reader =
      CreateGpuReader<GpuImageMeta>(
          listener_node, "camera/front", options,
          [&](GpuMsgView<GpuImageMeta>& view) {
            const auto& meta = *view;

            if (meta.width == 0 || meta.height == 0 ||
                meta.channels == 0) {
              AERROR << "Received invalid frame metadata";
              return;
            }

            if (meta.frame_id % 100 == 0) {
              AINFO << "Received frame " << meta.frame_id
                    << " [" << meta.width << "x"
                    << meta.height << "]"
                    << " GPU ptr: "
                    << view.device_ptr();
            }

            // 4. Submit downstream GPU work directly against the
            //    received device buffer.
            //
            // IMPORTANT:
            // Do not call Done() before all GPU consumers have been
            // submitted to infer_stream.
            //
            // In a real system this is where TensorRT inference,
            // CUDA preprocessing, feature extraction, encoding, etc.
            // would run.
            const size_t capacity = view.capacity();
            if (static_cast<size_t>(meta.width) >
                capacity / static_cast<size_t>(meta.height)) {
              AERROR << "Frame " << meta.frame_id
                     << " dimensions exceed the slot capacity";
              if (!view.Done(infer_stream)) {
                AERROR << "Failed to complete GPU frame "
                       << meta.frame_id;
              }
              return;
            }
            const size_t pixel_count =
                static_cast<size_t>(meta.width) * meta.height;
            if (static_cast<size_t>(meta.channels) >
                capacity / pixel_count) {
              AERROR << "Frame " << meta.frame_id
                     << " channels exceed the slot capacity";
              if (!view.Done(infer_stream)) {
                AERROR << "Failed to complete GPU frame "
                       << meta.frame_id;
              }
              return;
            }
            const size_t byte_size =
                pixel_count * static_cast<size_t>(meta.channels);

            // The reader exposes the transport buffer as const. Validate a
            // byte on the consumer stream without mutating shared storage.
            if (!ValidateGpuImage(
                    view.device_ptr(), byte_size,
                    static_cast<uint8_t>(meta.frame_id & 0xFF),
                    infer_stream)) {
              AERROR << "Failed to validate GPU frame " << meta.frame_id;
              if (!view.Done(infer_stream)) {
                AERROR << "Failed to complete GPU frame "
                       << meta.frame_id;
              }
              return;
            }

            if (!view.Done(infer_stream)) {
              AERROR << "Failed to complete GPU frame "
                     << meta.frame_id;
            }
          });

  if (!gpu_reader) {
    AERROR << "Failed to create GpuReader";
    cudaStreamDestroy(infer_stream);
    return -1;
  }

  AINFO << "GPU Listener initialized.";
  AINFO << "Waiting for GPU zero-copy frames.";

  apollo::cyber::WaitForShutdown();

  // Stop delivery before draining the stream so no callback can submit work
  // after the final synchronization.
  gpu_reader->Shutdown();

  if (!CheckCuda(
          cudaStreamSynchronize(infer_stream),
          "cudaStreamSynchronize(infer_stream)")) {
    AERROR << "Consumer CUDA stream did not synchronize cleanly";
  }

  if (!CheckCuda(
          cudaStreamDestroy(infer_stream),
          "cudaStreamDestroy(infer_stream)")) {
    return -1;
  }

  return 0;
}

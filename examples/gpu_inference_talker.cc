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

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <cuda_runtime.h>

#include "examples/gpu_image_meta.h"

#include "cyber/common/log.h"
#include "cyber/cyber.h"
#include "cyber/time/time.h"
#include "cyber/transport/nvsci/gpu_channel_manager.h"
#include "cyber/transport/nvsci/gpu_writer.h"

namespace {

using apollo::cyber::examples::GpuImageMeta;
using apollo::cyber::transport::GpuBackpressurePolicy;
using apollo::cyber::transport::GpuChannelManager;
using apollo::cyber::transport::GpuWriterOptions;

constexpr char kChannel[] = "camera/inference";
constexpr uint32_t kImageChannels = 3;

struct PpmImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
};

bool CheckCuda(cudaError_t error, const char* operation) {
  if (error == cudaSuccess) {
    return true;
  }
  AERROR << operation << " failed: " << cudaGetErrorString(error);
  return false;
}

bool ReadPpmToken(std::istream& input, std::string* token) {
  token->clear();
  char character = 0;
  while (input.get(character)) {
    if (std::isspace(static_cast<unsigned char>(character))) {
      continue;
    }
    if (character == '#') {
      input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      continue;
    }
    token->push_back(character);
    break;
  }
  if (token->empty()) {
    return false;
  }

  while (input.get(character)) {
    if (std::isspace(static_cast<unsigned char>(character))) {
      if (character == '\r' && input.peek() == '\n') {
        input.get();
      }
      return true;
    }
    token->push_back(character);
  }
  return false;
}

bool ParseUint32(const std::string& token, uint32_t* value) {
  uint32_t parsed = 0;
  const auto result =
      std::from_chars(token.data(), token.data() + token.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
    return false;
  }
  *value = parsed;
  return true;
}

bool ReadPpm(const std::string& path, PpmImage* image) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    AERROR << "Could not open PPM image: " << path;
    return false;
  }

  std::string magic;
  std::string width;
  std::string height;
  std::string max_value;
  if (!ReadPpmToken(input, &magic) || magic != "P6" ||
      !ReadPpmToken(input, &width) || !ReadPpmToken(input, &height) ||
      !ReadPpmToken(input, &max_value) || !ParseUint32(width, &image->width) ||
      !ParseUint32(height, &image->height)) {
    AERROR << "Expected a valid binary P6 PPM header in " << path;
    return false;
  }

  uint32_t parsed_max_value = 0;
  if (!ParseUint32(max_value, &parsed_max_value) || parsed_max_value != 255 ||
      image->width == 0 || image->height == 0) {
    AERROR << "PPM must have non-zero dimensions and max value 255";
    return false;
  }

  const size_t width_size = image->width;
  const size_t height_size = image->height;
  if (width_size > std::numeric_limits<size_t>::max() / height_size ||
      width_size * height_size >
          std::numeric_limits<size_t>::max() / kImageChannels) {
    AERROR << "PPM dimensions overflow the supported image size";
    return false;
  }
  const size_t byte_count = width_size * height_size * kImageChannels;
  if (byte_count >
      static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
    AERROR << "PPM image is too large to read";
    return false;
  }

  image->pixels.resize(byte_count);
  input.read(reinterpret_cast<char*>(image->pixels.data()),
             static_cast<std::streamsize>(byte_count));
  if (input.gcount() != static_cast<std::streamsize>(byte_count)) {
    AERROR << "PPM pixel data is truncated: " << path;
    return false;
  }
  return true;
}

bool ParseFrameCount(const char* text, uint32_t* frame_count) {
  uint32_t parsed = 0;
  const std::string value(text);
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      parsed == 0) {
    return false;
  }
  *frame_count = parsed;
  return true;
}

bool WaitForAllSlotsToBeFree(
    const std::shared_ptr<apollo::cyber::transport::GpuChannelSession>&
        session) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    bool all_free = true;
    for (uint32_t slot = 0; slot < session->pool()->GetSlotCount(); ++slot) {
      if (session->pool()->GetSlotState(static_cast<int>(slot)) !=
          apollo::cyber::transport::SlotState::FREE) {
        all_free = false;
        break;
      }
    }
    if (all_free) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (uint32_t slot = 0; slot < session->pool()->GetSlotCount(); ++slot) {
    const auto state = session->pool()->GetSlotState(static_cast<int>(slot));
    if (state != apollo::cyber::transport::SlotState::FREE) {
      AERROR << "GPU slot " << slot << " remained in state "
             << static_cast<int>(state);
    }
  }
  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 3) {
    AERROR << "Usage: gpu_inference_talker <image.ppm> [frame_count]";
    return 2;
  }

  uint32_t frame_count = 10;
  if (argc == 3 && !ParseFrameCount(argv[2], &frame_count)) {
    AERROR << "frame_count must be a positive uint32";
    return 2;
  }

  PpmImage image;
  if (!ReadPpm(argv[1], &image)) {
    return 2;
  }

  void* pinned_pixels = nullptr;
  if (!CheckCuda(cudaHostAlloc(&pinned_pixels, image.pixels.size(),
                               cudaHostAllocPortable),
                 "cudaHostAlloc")) {
    return 1;
  }
  std::memcpy(pinned_pixels, image.pixels.data(), image.pixels.size());

  cudaStream_t publish_stream = nullptr;
  if (!CheckCuda(
          cudaStreamCreateWithFlags(&publish_stream, cudaStreamNonBlocking),
          "cudaStreamCreateWithFlags")) {
    cudaFreeHost(pinned_pixels);
    return 1;
  }

  if (!apollo::cyber::Init(argv[0])) {
    cudaStreamDestroy(publish_stream);
    cudaFreeHost(pinned_pixels);
    return 1;
  }
  auto node = apollo::cyber::CreateNode("gpu_inference_talker");

  GpuWriterOptions options;
  options.slot_count = 4;
  options.slot_size = image.pixels.size();
  options.stream = publish_stream;
  options.backpressure = GpuBackpressurePolicy::TIMEOUT;
  options.timeout_ms = 5000;
  options.require_consumer = true;
  auto writer = apollo::cyber::transport::CreateGpuWriter<GpuImageMeta>(
      node, kChannel, options);
  if (!writer) {
    AERROR << "Could not create GPU writer";
    cudaStreamDestroy(publish_stream);
    cudaFreeHost(pinned_pixels);
    return 1;
  }

  auto session = GpuChannelManager::Instance()->GetSession(kChannel);
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (
      apollo::cyber::OK() &&
      std::chrono::steady_clock::now() < ready_deadline &&
      (!writer->HasReader() || !session || session->GetConsumerCount() == 0)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!writer->HasReader() || !session || session->GetConsumerCount() == 0) {
    AERROR << "No GPU listener registered on " << kChannel;
    writer->Shutdown();
    cudaStreamDestroy(publish_stream);
    cudaFreeHost(pinned_pixels);
    return 1;
  }
  AINFO << "Publishing " << frame_count << " RGB8 frames (" << image.width
        << "x" << image.height << ") from " << argv[1] << " on " << kChannel;

  bool success = true;
  for (uint32_t frame_id = 0; frame_id < frame_count; ++frame_id) {
    auto loan = writer->Loan(publish_stream);
    if (!loan) {
      AERROR << "Timed out waiting for a GPU buffer slot";
      success = false;
      break;
    }

    if (image.pixels.size() > loan->capacity()) {
      AERROR << "Image exceeds the GPU buffer slot capacity";
      success = false;
      break;
    }
    if (!CheckCuda(cudaMemcpyAsync(loan->device_ptr(), pinned_pixels,
                                   image.pixels.size(), cudaMemcpyHostToDevice,
                                   publish_stream),
                   "cudaMemcpyAsync(image upload)")) {
      success = false;
      break;
    }

    loan->metadata().width = image.width;
    loan->metadata().height = image.height;
    loan->metadata().channels = kImageChannels;
    loan->metadata().frame_id = frame_id;
    loan->metadata().timestamp_ns = apollo::cyber::Time::Now().ToNanosecond();
    if (!writer->Publish(std::move(*loan))) {
      AERROR << "Failed to publish frame " << frame_id;
      success = false;
      break;
    }
    AINFO << "Published frame " << frame_id;
    if (frame_id == 0 && frame_count > 1 && !WaitForAllSlotsToBeFree(session)) {
      AERROR << "Timed out waiting for the initial GPU inference frame";
      success = false;
      break;
    }
  }

  success = CheckCuda(cudaStreamSynchronize(publish_stream),
                      "cudaStreamSynchronize(publish_stream)") &&
            success;
  if (success && !WaitForAllSlotsToBeFree(session)) {
    AERROR << "Timed out waiting for listener inference completion";
    success = false;
  }

  writer->Shutdown();
  success = CheckCuda(cudaStreamDestroy(publish_stream),
                      "cudaStreamDestroy(publish_stream)") &&
            success;
  success = CheckCuda(cudaFreeHost(pinned_pixels), "cudaFreeHost") && success;
  return success ? 0 : 1;
}

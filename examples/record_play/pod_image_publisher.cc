// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cyber/cyber.h"
#include "cyber/transport/message/pod_message.h"

namespace {

constexpr char kDefaultChannel[] = "/example/sensor/camera";
constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr uint32_t kStride = kWidth * 3;
constexpr std::size_t kDefaultBytes =
    static_cast<std::size_t>(kStride) * kHeight;

struct Options {
  std::string channel = kDefaultChannel;
  std::size_t bytes = kDefaultBytes;
  int count = 10;
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto equal = arg.find('=');
    const auto key = arg.substr(0, equal);
    const auto value =
        equal == std::string::npos ? std::string() : arg.substr(equal + 1);
    if (key == "--channel" && !value.empty()) {
      options.channel = value;
    } else if (key == "--bytes" && !value.empty()) {
      options.bytes = std::stoull(value);
    } else if (key == "--count" && !value.empty()) {
      options.count = std::stoi(value);
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  apollo::cyber::Init(argv[0]);
  const auto options = ParseOptions(argc, argv);
  auto node = apollo::cyber::CreateNode("pod_image_publisher");
  auto writer =
      node->CreateWriter<apollo::cyber::transport::PodMessage>(options.channel);
  if (writer == nullptr) {
    std::cerr << "failed to create POD writer" << std::endl;
    apollo::cyber::Clear();
    return 1;
  }

  while (apollo::cyber::OK() && !writer->HasReader()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const std::vector<uint8_t> payload(options.bytes, 0x5A);
  for (int i = 0; i < options.count && apollo::cyber::OK(); ++i) {
    apollo::cyber::Writer<apollo::cyber::transport::PodMessage>::LoanedMessage
        loaned;
    if (!writer->Loan(
            apollo::cyber::transport::PodChunkTotalSize(payload.size()),
            &loaned)) {
      std::cerr << "POD loan unavailable; use an Iceoryx/SHM reader"
                << std::endl;
      apollo::cyber::Clear();
      return 1;
    }

    const auto header = apollo::cyber::transport::MakeImagePodChunkHeader(
        apollo::cyber::Time::Now().ToNanosecond(), static_cast<uint64_t>(i),
        kWidth, kHeight, kStride, 1,
        static_cast<uint32_t>(payload.size()));
    std::size_t written = 0;
    if (!apollo::cyber::transport::BuildPodChunk(
            header, payload.data(), payload.size(), loaned.data(),
            loaned.capacity(), &written) ||
        !loaned.set_size(written) || !writer->Publish(std::move(loaned))) {
      std::cerr << "POD image publish failed" << std::endl;
      apollo::cyber::Clear();
      return 1;
    }
  }

  apollo::cyber::Clear();
  return 0;
}

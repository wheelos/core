// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

#include "cyber/cyber.h"
#include "cyber/transport/message/pod_message.h"

namespace {

constexpr char kDefaultChannel[] = "/example/sensor/camera";

std::string ParseChannel(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    constexpr char kPrefix[] = "--channel=";
    if (arg.rfind(kPrefix, 0) == 0) {
      return arg.substr(sizeof(kPrefix) - 1);
    }
  }
  return kDefaultChannel;
}

}  // namespace

int main(int argc, char** argv) {
  apollo::cyber::Init(argv[0]);
  const auto channel = ParseChannel(argc, argv);
  auto node = apollo::cyber::CreateNode("pod_image_subscriber");
  auto reader = node->CreateReader<apollo::cyber::transport::PodMessage>(
      channel,
      [](const std::shared_ptr<apollo::cyber::transport::PodMessage>& message) {
        const auto* header = message->header();
        const auto view = message->View();
        if (header == nullptr || view.payload == nullptr ||
            view.payload_size != header->payload_size ||
            header->payload_kind != static_cast<uint32_t>(
                                        apollo::cyber::transport::PodPayloadKind::IMAGE)) {
          std::cerr << "invalid POD image" << std::endl;
          return;
        }
        std::cout << "image frame=" << header->frame_id
                  << " bytes=" << view.payload_size
                  << " borrowed=" << (message->is_borrowed() ? "true" : "false")
                  << std::endl;
      });
  if (reader == nullptr) {
    std::cerr << "failed to create POD image reader" << std::endl;
    apollo::cyber::Clear();
    return 1;
  }

  apollo::cyber::WaitForShutdown();
  apollo::cyber::Clear();
  return 0;
}

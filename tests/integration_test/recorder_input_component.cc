// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#include "tests/integration_test/recorder_input_component.h"

#include <cstdlib>

bool RecorderInputComponent::Init() {
  const char* output_channel = std::getenv("CYBER_TEST_ACK_CHANNEL");
  const char* token = std::getenv("CYBER_TEST_COMPONENT_TOKEN");
  if (output_channel == nullptr || token == nullptr) {
    return false;
  }
  token_ = token;
  writer_ = node_->CreateWriter<apollo::cyber::examples::proto::Chatter>(
      output_channel);
  return writer_ != nullptr;
}

bool RecorderInputComponent::Proc(
    const std::shared_ptr<apollo::cyber::examples::proto::Chatter>& message) {
  auto response =
      std::make_shared<apollo::cyber::examples::proto::Chatter>(*message);
  response->set_content(token_ + ":" + message->content());
  return writer_->Write(response);
}

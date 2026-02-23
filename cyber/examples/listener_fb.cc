/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
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

/**
 * @file listener_fb.cc
 * @brief FlatBuffers-based listener demonstrating zero-copy message access.
 *
 * This example mirrors listener.cc but subscribes to FlatBufferMessage
 * payloads.  On receipt the callback calls GetRoot<Chatter>() to access the
 * FlatBuffers root object directly from the buffer — no copy, no allocation.
 */

#include "cyber/cyber.h"
#include "cyber/examples/proto/examples_generated.h"
#include "cyber/message/flatbuffers_message.h"

using apollo::cyber::message::FlatBufferMessage;
using apollo::cyber::examples::proto::Chatter;

void MessageCallback(const std::shared_ptr<FlatBufferMessage>& msg) {
  // Zero-copy: access the FlatBuffers root directly from the buffer.
  const Chatter* chatter = msg->GetRoot<Chatter>();
  if (chatter == nullptr) {
    AERROR << "listener_fb: received an empty FlatBufferMessage";
    return;
  }
  AINFO << "listener_fb received seq->" << chatter->seq();
  if (chatter->content()) {
    AINFO << "listener_fb content->" << chatter->content()->str();
  }
}

int main(int argc, char* argv[]) {
  apollo::cyber::Init(argv[0]);

  auto listener_node = apollo::cyber::CreateNode("listener_fb");
  auto listener = listener_node->CreateReader<FlatBufferMessage>(
      "channel/chatter_fb", MessageCallback);

  apollo::cyber::WaitForShutdown();
  return 0;
}

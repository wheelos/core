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

#include "cyber/cyber.h"
#include "cyber/examples/proto/examples_generated.h"
#include "cyber/message/flatbuffers_message.h"

using apollo::cyber::examples::proto::Chatter;
using apollo::cyber::message::FlatBufferMessage;

void MessageCallback(const std::shared_ptr<FlatBufferMessage>& msg) {
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

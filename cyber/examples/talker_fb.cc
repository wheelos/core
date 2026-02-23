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
 * @file talker_fb.cc
 * @brief FlatBuffers-based talker demonstrating zero-copy publishing.
 *
 * This example mirrors talker.cc but uses FlatBufferMessage instead of a
 * Protobuf-generated Chatter.  The payload is serialized once into a
 * FlatBufferBuilder and then wrapped in a FlatBufferMessage for publishing.
 * Because the cyber shared-memory transport copies the raw byte buffer
 * directly into SHM, the receiver can access the FlatBuffers root object
 * without any additional deserialization.
 */

#include "cyber/cyber.h"
#include "cyber/examples/proto/examples_generated.h"
#include "cyber/message/flatbuffers_message.h"
#include "cyber/time/rate.h"
#include "cyber/time/time.h"

using apollo::cyber::Rate;
using apollo::cyber::Time;
using apollo::cyber::message::FlatBufferMessage;
using apollo::cyber::examples::proto::CreateChatterDirect;

int main(int argc, char* argv[]) {
  apollo::cyber::Init(argv[0]);

  auto talker_node = apollo::cyber::CreateNode("talker_fb");
  auto talker = talker_node->CreateWriter<FlatBufferMessage>("channel/chatter_fb");

  Rate rate(1.0);
  uint64_t seq = 0;

  while (apollo::cyber::OK()) {
    flatbuffers::FlatBufferBuilder fbb;
    auto chatter = CreateChatterDirect(
        fbb,
        /*timestamp=*/Time::Now().ToNanosecond(),
        /*lidar_timestamp=*/Time::Now().ToNanosecond(),
        /*seq=*/seq,
        /*content=*/"Hello, FlatBuffers!");
    fbb.Finish(chatter);

    auto msg = std::make_shared<FlatBufferMessage>(
        "apollo.cyber.examples.proto.Chatter",
        fbb.GetBufferPointer(), fbb.GetSize());
    msg->set_timestamp(Time::Now().ToNanosecond());

    talker->Write(msg);
    AINFO << "talker_fb sent a FlatBuffers message! No. " << seq;
    ++seq;
    rate.Sleep();
  }
  return 0;
}

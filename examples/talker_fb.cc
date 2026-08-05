// Copyright 2025 WheelOS. All Rights Reserved.
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


#include "cyber/cyber.h"
#include "examples/proto/examples_generated.h"
#include "cyber/message/flatbuffers_message.h"
#include "cyber/proto/role_attributes.pb.h"
#include "cyber/time/rate.h"
#include "cyber/time/time.h"

using apollo::cyber::Rate;
using apollo::cyber::Time;
using apollo::cyber::examples::proto::CreateChatterDirect;
using apollo::cyber::message::FlatBufferMessage;

namespace {

constexpr char kChannelName[] = "channel/chatter_fb";
constexpr char kChatterMessageType[] = "apollo.cyber.examples.proto.Chatter";

}  // namespace

int main(int argc, char* argv[]) {
  apollo::cyber::Init(argv[0]);

  auto talker_node = apollo::cyber::CreateNode("talker_fb");
  apollo::cyber::proto::RoleAttributes role_attr;
  role_attr.set_channel_name(kChannelName);
  role_attr.set_message_type(kChatterMessageType);
  auto talker = talker_node->CreateWriter<FlatBufferMessage>(role_attr);

  Rate rate(1.0);
  uint64_t seq = 0;

  while (apollo::cyber::OK()) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto now = Time::Now().ToNanosecond();
    auto chatter = CreateChatterDirect(
        fbb, /*timestamp=*/now, /*lidar_timestamp=*/now, /*seq=*/seq,
        /*content=*/"Hello, FlatBuffers!");
    fbb.Finish(chatter);
    auto msg = std::make_shared<FlatBufferMessage>(
        kChatterMessageType, fbb.GetBufferPointer(), fbb.GetSize());
    msg->set_timestamp(now);

    talker->Write(msg);
    AINFO << "talker_fb sent a FlatBuffers message! No. " << seq;
    ++seq;
    rate.Sleep();
  }
  return 0;
}

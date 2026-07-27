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

#include "cyber/node/node.h"

#include "gtest/gtest.h"

#include "cyber/proto/unit_test.pb.h"

#include "cyber/cyber.h"
#include "cyber/init.h"
#include "cyber/node/reader.h"
#include "cyber/node/writer.h"

#include <cstdio>

namespace apollo {
namespace cyber {

using apollo::cyber::proto::Chatter;

TEST(NodeTest, cases) {
  auto node = CreateNode("node_test");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), "node_test");

  proto::RoleAttributes attr;
  attr.set_channel_name("/node_test_channel");
  auto channel_id = common::GlobalData::RegisterChannel(attr.channel_name());
  attr.set_channel_id(channel_id);
  attr.mutable_qos_profile()->set_depth(10);

  auto reader = node->CreateReader<Chatter>(attr);
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(node->GetReader<Chatter>(attr.channel_name()));

  auto writer = node->CreateWriter<Chatter>(attr);
  ASSERT_NE(writer, nullptr);
  auto server = node->CreateService<Chatter, Chatter>(
      "node_test_server", [](const std::shared_ptr<Chatter>& request,
                             std::shared_ptr<Chatter>& response) {
        AINFO << "server: I am server";
        static uint64_t id = 0;
        ++id;
        response->set_seq(id);
        response->set_timestamp(0);
      });
  ASSERT_NE(server, nullptr);
  auto client = node->CreateClient<Chatter, Chatter>("node_test_server");
  ASSERT_NE(client, nullptr);
  auto chatter_msg = std::make_shared<Chatter>();
  chatter_msg->set_seq(0);
  chatter_msg->set_timestamp(0);
  auto res = client->SendRequest(chatter_msg);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->seq(), 1);

  node->Observe();
  node->ClearData();
}

}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  apollo::cyber::Init(argv[0]);
  const int ret = RUN_ALL_TESTS();
  std::fflush(nullptr);
  _Exit(ret);
}

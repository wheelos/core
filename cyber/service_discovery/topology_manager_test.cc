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

#include "cyber/service_discovery/topology_manager.h"

#include <memory>

#include "gtest/gtest.h"

#include "cyber/common/global_data.h"
#include "cyber/common/log.h"
#include "cyber/transport/common/identity.h"

namespace apollo {
namespace cyber {
namespace service_discovery {

class TopologyTest : public ::testing::Test {
 protected:
  TopologyTest() { topology_ = TopologyManager::Instance(); }
  virtual ~TopologyTest() {}

  virtual void SetUp() {}

  virtual void TearDown() {}

  TopologyManager* topology_;
};

TEST_F(TopologyTest, add_and_remove_change_listener) {
  proto::RoleAttributes attr;
  attr.set_host_name("");
  attr.set_process_id(0);

  // add change listener
  auto conn =
      topology_->AddChangeListener([&attr](const ChangeMsg& change_msg) {
        if (change_msg.change_type() == ChangeType::CHANGE_PARTICIPANT &&
            change_msg.operate_type() == OperateType::OPT_JOIN &&
            change_msg.role_type() == RoleType::ROLE_PARTICIPANT) {
          attr.CopyFrom(change_msg.role_attr());
        }
      });

  // remove change listener
  topology_->RemoveChangeListener(conn);
}

TEST_F(TopologyTest, get_manager) {
  auto node_manager = topology_->node_manager();
  EXPECT_NE(node_manager, nullptr);

  auto channel_manager = topology_->channel_manager();
  EXPECT_NE(channel_manager, nullptr);

  auto service_manager = topology_->service_manager();
  EXPECT_NE(service_manager, nullptr);
}

TEST_F(TopologyTest, recursive_publish_in_change_listener) {
  std::atomic<bool> reentrant_called{false};
  proto::RoleAttributes reader_attr;
  reader_attr.set_host_name(common::GlobalData::Instance()->HostName());
  reader_attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  reader_attr.set_node_name("recursive_test_reader_node");
  reader_attr.set_node_id(common::GlobalData::RegisterNode(reader_attr.node_name()));
  reader_attr.set_channel_name("recursive_test_reader_channel");
  reader_attr.set_channel_id(
      common::GlobalData::Instance()->RegisterChannel(reader_attr.channel_name()));
  transport::Identity reader_id;
  reader_attr.set_id(reader_id.HashValue());

  auto conn = topology_->channel_manager()->AddChangeListener(
      [this, &reader_attr, &reentrant_called](const ChangeMsg& msg) {
        if (msg.role_type() == RoleType::ROLE_WRITER &&
            msg.operate_type() == OperateType::OPT_JOIN &&
            msg.role_attr().channel_name() == "trigger_channel") {
          // Join a new reader synchronously inside the topology callback.
          // In FastDDS 2.14 intraprocess mode, this writes to the discovery
          // topic and re-enters SubscriberListener on the same thread.
          topology_->channel_manager()->Join(reader_attr, RoleType::ROLE_READER);
          reentrant_called.store(true);
        }
      });

  proto::RoleAttributes writer_attr;
  writer_attr.set_host_name(common::GlobalData::Instance()->HostName());
  writer_attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  writer_attr.set_node_name("trigger_writer_node");
  writer_attr.set_node_id(common::GlobalData::RegisterNode(writer_attr.node_name()));
  writer_attr.set_channel_name("trigger_channel");
  writer_attr.set_channel_id(
      common::GlobalData::Instance()->RegisterChannel(writer_attr.channel_name()));
  transport::Identity writer_id;
  writer_attr.set_id(writer_id.HashValue());

  EXPECT_TRUE(topology_->channel_manager()->Join(writer_attr, RoleType::ROLE_WRITER));
  EXPECT_TRUE(reentrant_called.load());

  topology_->channel_manager()->RemoveChangeListener(conn);
  topology_->channel_manager()->Leave(writer_attr, RoleType::ROLE_WRITER);
  topology_->channel_manager()->Leave(reader_attr, RoleType::ROLE_READER);
}

}  // namespace service_discovery
}  // namespace cyber
}  // namespace apollo

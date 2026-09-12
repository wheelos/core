// Copyright 2026 WheelOS. All Rights Reserved.
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

#include "cyber/transport/nvsci/gpu_loaned_message.h"

#include <gtest/gtest.h>

namespace apollo {
namespace cyber {
namespace transport {

struct TestImageMeta {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t channels = 3;
  uint64_t timestamp = 0;
};

TEST(GpuLoanedMessageTest, ProducerLoanAndPublish) {
  NvSciBufPoolConfig config;
  config.slot_count = 2;
  config.slot_size = 1920 * 1080 * 3;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  auto sync_engine = std::make_shared<NvSciSyncEngine>(1);
  auto session = std::make_shared<GpuChannelSession>(5001, pool, sync_engine);

  session->RegisterConsumer(100);

  int slot = pool->AcquireSlot();
  EXPECT_EQ(slot, 0);

  TestImageMeta meta;
  meta.timestamp = 123456789;

  GpuTransportPacket packet;
  {
    GpuLoanedMessage<TestImageMeta> loaned_msg(slot, pool->GetDevicePtr(slot),
                                               &meta, session);
    EXPECT_TRUE(loaned_msg.is_valid());
    EXPECT_EQ(loaned_msg.slot_id(), 0);
    EXPECT_NE(loaned_msg.device_ptr(), nullptr);
    EXPECT_EQ(loaned_msg->width, 1920);
    EXPECT_EQ(loaned_msg->timestamp, 123456789);

    EXPECT_TRUE(loaned_msg.Publish(&packet));
  }

  // After publishing and leaving scope, slot remains IN_USE by consumer 100
  EXPECT_EQ(pool->GetSlotState(0), SlotState::IN_USE);

  // Consumer receives packet and creates GpuConstView
  {
    GpuConstView<TestImageMeta> view(packet.channel_id, packet.slot_id,
                                     pool->GetDevicePtr(packet.slot_id), &meta,
                                     packet.prefence, 100, session,
                                     packet.seq_num);
    EXPECT_EQ(view.slot_id(), 0);
    EXPECT_EQ(view.seq_num(), packet.seq_num);
    EXPECT_EQ(view->width, 1920);
    EXPECT_TRUE(view.WaitUntilReady(nullptr));

    // Signal completion
    EXPECT_TRUE(view.SignalCompletion(nullptr));
    EXPECT_TRUE(view.is_completed());
  }

  // Slot should now be FREE
  EXPECT_EQ(pool->GetSlotState(0), SlotState::FREE);
}

TEST(GpuLoanedMessageTest, LoanCancellationOnUnpublishedDestruction) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  auto sync_engine = std::make_shared<NvSciSyncEngine>(1);
  auto session = std::make_shared<GpuChannelSession>(5002, pool, sync_engine);

  int slot = pool->AcquireSlot();
  EXPECT_EQ(slot, 0);
  EXPECT_EQ(pool->GetSlotState(0), SlotState::LOANED);

  TestImageMeta meta;
  {
    GpuLoanedMessage<TestImageMeta> msg(slot, pool->GetDevicePtr(slot), &meta,
                                        session);
    // Destructor called without Publish() -> should cancel loan and free slot
  }

  EXPECT_EQ(pool->GetSlotState(0), SlotState::FREE);
}

TEST(GpuLoanedMessageTest, LoanAcquiresSlotFromSession) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  auto sync_engine = std::make_shared<NvSciSyncEngine>(3);
  auto session = std::make_shared<GpuChannelSession>(5003, pool, sync_engine);
  session->RegisterConsumer(300);

  TestImageMeta meta;
  GpuTransportPacket packet;
  {
    GpuLoanedMessage<TestImageMeta> message(&meta, session);
    ASSERT_TRUE(message.is_valid());
    EXPECT_EQ(message.slot_id(), 0);
    ASSERT_TRUE(message.Publish(&packet));
  }

  GpuConstView<TestImageMeta> view(packet, &meta, 300, session);
  ASSERT_TRUE(view.SignalCompletion(nullptr));
  EXPECT_EQ(pool->GetSlotState(0), SlotState::FREE);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

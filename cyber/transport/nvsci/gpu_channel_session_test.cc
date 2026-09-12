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

#include "cyber/transport/nvsci/gpu_channel_session.h"

#include <gtest/gtest.h>

namespace apollo {
namespace cyber {
namespace transport {

TEST(GpuChannelSessionTest, FanoutAndCompletion) {
  NvSciBufPoolConfig config;
  config.slot_count = 2;
  config.slot_size = 1024;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  auto sync_engine = std::make_shared<NvSciSyncEngine>(1);
  GpuChannelSession session(1001, pool, sync_engine);

  // Register 2 consumers (1:2 fanout)
  session.RegisterConsumer(201);
  session.RegisterConsumer(202);
  EXPECT_EQ(session.GetConsumerCount(), 2);

  int slot = pool->AcquireSlot();
  EXPECT_EQ(slot, 0);

  NvSciSyncFence prefence = sync_engine->GenerateSignalFence(nullptr);
  GpuTransportPacket packet;
  EXPECT_TRUE(session.OnPublish(slot, prefence, &packet));
  EXPECT_EQ(packet.channel_id, 1001);
  EXPECT_EQ(packet.slot_id, 0);
  EXPECT_EQ(pool->GetSlotState(0), SlotState::IN_USE);

  // First consumer completes
  GpuCompletionPacket comp1;
  comp1.channel_id = 1001;
  comp1.slot_id = 0;
  comp1.seq_num = packet.seq_num;
  comp1.consumer_id = 201;
  comp1.postfence = sync_engine->GenerateSignalFence(nullptr);

  EXPECT_TRUE(session.OnCompletion(comp1));
  // Slot should still be IN_USE because consumer 202 has not completed
  EXPECT_EQ(pool->GetSlotState(0), SlotState::IN_USE);

  // Second consumer completes
  GpuCompletionPacket comp2;
  comp2.channel_id = 1001;
  comp2.slot_id = 0;
  comp2.seq_num = packet.seq_num;
  comp2.consumer_id = 202;
  comp2.postfence = sync_engine->GenerateSignalFence(nullptr);

  EXPECT_TRUE(session.OnCompletion(comp2));
  // Now slot should return to FREE
  EXPECT_EQ(pool->GetSlotState(0), SlotState::FREE);
}

TEST(GpuChannelSessionTest, ReapHungConsumer) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  auto sync_engine = std::make_shared<NvSciSyncEngine>(1);
  GpuChannelSession session(1002, pool, sync_engine);

  session.RegisterConsumer(301);
  int slot = pool->AcquireSlot();
  EXPECT_EQ(slot, 0);

  GpuTransportPacket packet;
  NvSciSyncFence prefence = sync_engine->GenerateSignalFence(nullptr);
  EXPECT_TRUE(session.OnPublish(slot, prefence, &packet));
  EXPECT_EQ(pool->GetSlotState(0), SlotState::IN_USE);

  // Immediate reap with 10s timeout should reap 0 slots
  EXPECT_EQ(session.ReapHungSlots(10000000000ULL), 0);
  EXPECT_EQ(pool->GetSlotState(0), SlotState::IN_USE);

  // Reap with 0 timeout (immediate expiration) -> slot is quarantined safely
  EXPECT_EQ(session.ReapHungSlots(0), 1);
  EXPECT_EQ(pool->GetSlotState(0), SlotState::QUARANTINED);

  // Before fence completion, slot cannot be recovered
  EXPECT_EQ(session.RecoverQuarantinedSlots(), 0);
  EXPECT_EQ(pool->GetSlotState(0), SlotState::QUARANTINED);

  // Mark fence completed
  sync_engine->MarkFenceCompleted(prefence.fence_id);

  // Recover quarantined slot once fence is signaled
  EXPECT_EQ(session.RecoverQuarantinedSlots(), 1);
  EXPECT_EQ(pool->GetSlotState(0), SlotState::FREE);
}

TEST(GpuChannelSessionTest, StrictSequenceMatchingRejectsStaleCompletions) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  auto sync_engine = std::make_shared<NvSciSyncEngine>(1);
  GpuChannelSession session(1003, pool, sync_engine);

  session.RegisterConsumer(401);
  int slot = pool->AcquireSlot();
  EXPECT_EQ(slot, 0);

  GpuTransportPacket packet;
  NvSciSyncFence prefence = sync_engine->GenerateSignalFence(nullptr);
  EXPECT_TRUE(session.OnPublish(slot, prefence, &packet));
  EXPECT_GT(packet.seq_num, 0);

  // Stale completion with seq_num = 0 or old seq_num must be rejected
  GpuCompletionPacket stale_comp;
  stale_comp.channel_id = 1003;
  stale_comp.slot_id = 0;
  stale_comp.seq_num = 0;  // zero / uninitialized seq_num
  stale_comp.consumer_id = 401;
  EXPECT_FALSE(session.OnCompletion(stale_comp));
  EXPECT_EQ(pool->GetSlotState(0), SlotState::IN_USE);

  stale_comp.seq_num = packet.seq_num + 999;  // wrong seq_num
  EXPECT_FALSE(session.OnCompletion(stale_comp));
  EXPECT_EQ(pool->GetSlotState(0), SlotState::IN_USE);

  // Correct seq_num completes normally
  stale_comp.seq_num = packet.seq_num;
  EXPECT_TRUE(session.OnCompletion(stale_comp));
  EXPECT_EQ(pool->GetSlotState(0), SlotState::FREE);
}

TEST(GpuChannelSessionTest, CancelPublishReleasesEveryConsumerReference) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  auto sync_engine = std::make_shared<NvSciSyncEngine>(1);
  GpuChannelSession session(1004, pool, sync_engine);
  session.RegisterConsumer(501);
  session.RegisterConsumer(502);

  const int slot = session.AcquireSlot();
  ASSERT_EQ(slot, 0);
  GpuTransportPacket packet;
  ASSERT_TRUE(session.OnPublish(slot, sync_engine->GenerateSignalFence(nullptr),
                                &packet));
  ASSERT_EQ(pool->GetSlotState(slot), SlotState::IN_USE);

  EXPECT_TRUE(session.CancelPublish(slot, packet.seq_num));
  EXPECT_EQ(pool->GetSlotState(slot), SlotState::FREE);
  EXPECT_FALSE(session.CancelPublish(slot, packet.seq_num));
}

TEST(GpuChannelSessionTest, ReusesSlotOnlyAfterPostFenceCompletes) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  auto sync_engine = std::make_shared<NvSciSyncEngine>(18);
  GpuChannelSession session(1006, pool, sync_engine);
  session.RegisterConsumer(701);

  const int slot = session.AcquireSlot();
  ASSERT_EQ(slot, 0);
  GpuTransportPacket packet;
  ASSERT_TRUE(session.OnPublish(
      slot, sync_engine->GenerateSignalFence(nullptr), &packet));

  GpuCompletionPacket completion;
  completion.channel_id = session.channel_id();
  completion.slot_id = static_cast<uint32_t>(slot);
  completion.seq_num = packet.seq_num;
  completion.consumer_id = 701;
  completion.postfence = sync_engine->GenerateSignalFence(nullptr);
  ASSERT_TRUE(session.OnCompletion(completion));
  ASSERT_EQ(pool->GetSlotState(slot), SlotState::FREE);

  EXPECT_EQ(session.AcquireSlot(), -1);
  sync_engine->MarkFenceCompleted(completion.postfence.fence_id);
  EXPECT_EQ(session.AcquireSlot(), slot);
}

TEST(GpuChannelSessionTest, ConsumerCrashAndRestartReclaimsInFlightSlot) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());
  auto sync_engine = std::make_shared<NvSciSyncEngine>(17);
  GpuChannelSession session(1005, pool, sync_engine);

  session.RegisterConsumer(601);
  const int first_slot = session.AcquireSlot();
  ASSERT_EQ(first_slot, 0);
  GpuTransportPacket first_packet;
  ASSERT_TRUE(session.OnPublish(
      first_slot, sync_engine->GenerateSignalFence(nullptr), &first_packet));

  // Process death removes its outstanding ownership. A restarted process may
  // safely register the same logical consumer and receive a fresh sequence.
  session.UnregisterConsumer(601);
  EXPECT_EQ(pool->GetSlotState(first_slot), SlotState::FREE);
  EXPECT_TRUE(pool->GetLastPostFences(first_slot).empty());
  session.RegisterConsumer(601);

  const int restarted_slot = session.AcquireSlot();
  ASSERT_EQ(restarted_slot, 0);
  GpuTransportPacket restarted_packet;
  ASSERT_TRUE(session.OnPublish(restarted_slot,
                                sync_engine->GenerateSignalFence(nullptr),
                                &restarted_packet));
  EXPECT_GT(restarted_packet.seq_num, first_packet.seq_num);

  GpuCompletionPacket stale;
  stale.channel_id = session.channel_id();
  stale.slot_id = 0;
  stale.seq_num = first_packet.seq_num;
  stale.consumer_id = 601;
  EXPECT_FALSE(session.OnCompletion(stale));

  stale.seq_num = restarted_packet.seq_num;
  EXPECT_TRUE(session.OnCompletion(stale));
  EXPECT_EQ(pool->GetSlotState(0), SlotState::FREE);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

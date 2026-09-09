/******************************************************************************
 * Copyright 2026 WheelOS. All Rights Reserved.
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

#include "cyber/transport/nvsci/nvsci_buf_pool.h"

#include <gtest/gtest.h>

namespace apollo {
namespace cyber {
namespace transport {

TEST(NvSciBufPoolTest, InitializationAndAcquisition) {
  NvSciBufPoolConfig config;
  config.slot_count = 3;
  config.slot_size = 1024 * 1024;
  config.alignment = 4096;

  NvSciBufPool pool(config);
  EXPECT_TRUE(pool.Initialize());
  EXPECT_EQ(pool.GetSlotCount(), 3);
  EXPECT_EQ(pool.GetSlotCapacity(), 1024 * 1024);

  int slot0 = pool.AcquireSlot();
  int slot1 = pool.AcquireSlot();
  int slot2 = pool.AcquireSlot();

  EXPECT_EQ(slot0, 0);
  EXPECT_EQ(slot1, 1);
  EXPECT_EQ(slot2, 2);

  // All slots in use, next acquire must return -1
  EXPECT_EQ(pool.AcquireSlot(), -1);

  EXPECT_EQ(pool.GetSlotState(0), SlotState::LOANED);
  EXPECT_EQ(pool.GetSlotState(1), SlotState::LOANED);
  EXPECT_EQ(pool.GetSlotState(2), SlotState::LOANED);

  // Check alignment
  void* ptr0 = pool.GetDevicePtr(0);
  EXPECT_NE(ptr0, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr0) % config.alignment, 0);

  // Mark in use
  EXPECT_TRUE(pool.MarkInUse(0, 2));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::IN_USE);

  // Release first ref
  NvSciSyncFence fence;
  fence.fence_id = 123;
  EXPECT_TRUE(pool.ReleaseSlot(0, fence));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::IN_USE);

  // Release second ref -> should become FREE
  EXPECT_TRUE(pool.ReleaseSlot(0, fence));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::FREE);

  // Now we can acquire again
  int slot_reacquired = pool.AcquireSlot();
  EXPECT_EQ(slot_reacquired, 0);
}

TEST(NvSciBufPoolTest, ExportAndImport) {
  NvSciBufPoolConfig config;
  config.slot_count = 2;
  config.slot_size = 512 * 1024;

  NvSciBufPool pool(config);
  EXPECT_TRUE(pool.Initialize());

  std::vector<uint8_t> ipc_desc;
  EXPECT_TRUE(pool.ExportBuffer(1, &ipc_desc));
  EXPECT_FALSE(ipc_desc.empty());

  EXPECT_TRUE(pool.ImportBuffer(1, ipc_desc));
  // Mismatched slot id import should fail
  EXPECT_FALSE(pool.ImportBuffer(0, ipc_desc));
}

TEST(NvSciBufPoolTest, QuarantineAndUnquarantine) {
  NvSciBufPoolConfig config;
  config.slot_count = 2;
  config.slot_size = 1024;

  NvSciBufPool pool(config);
  EXPECT_TRUE(pool.Initialize());

  int slot = pool.AcquireSlot();
  EXPECT_EQ(slot, 0);
  EXPECT_TRUE(pool.MarkInUse(0, 1));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::IN_USE);

  EXPECT_TRUE(pool.QuarantineSlot(0));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::QUARANTINED);

  // While quarantined, AcquireSlot must not hand out slot 0
  int next_slot = pool.AcquireSlot();
  EXPECT_EQ(next_slot, 1);
  EXPECT_EQ(pool.AcquireSlot(), -1);

  // Unquarantine restores it to FREE
  EXPECT_TRUE(pool.UnquarantineSlot(0));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::FREE);
  EXPECT_EQ(pool.AcquireSlot(), 0);
}

TEST(NvSciBufPoolTest, RejectsInvalidReferenceTransitions) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  NvSciBufPool pool(config);
  ASSERT_TRUE(pool.Initialize());

  EXPECT_EQ(pool.AcquireSlot(), 0);
  EXPECT_FALSE(pool.MarkInUse(0, 0));
  EXPECT_TRUE(pool.MarkInUse(0, 1));
  EXPECT_TRUE(pool.QuarantineSlot(0));

  NvSciSyncFence fence;
  fence.fence_id = 7;
  EXPECT_FALSE(pool.ReleaseSlot(0, fence));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::QUARANTINED);
  EXPECT_TRUE(pool.UnquarantineSlot(0));
  EXPECT_FALSE(pool.ReleaseSlot(0, fence));
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

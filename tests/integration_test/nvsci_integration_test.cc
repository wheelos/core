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

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cyber/transport/nvsci/gpu_channel_session.h"
#include "cyber/transport/nvsci/gpu_loaned_message.h"

#if defined(CYBER_USE_CUDA_IPC)
#include <cuda_runtime.h>
#endif

namespace apollo {
namespace cyber {
namespace transport {

struct CameraFrameMeta {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t frame_id = 0;
  uint64_t timestamp_ns = 0;
};

TEST(NvSciIntegrationTest, MultiFramePipelineFanout1To2) {
  constexpr uint32_t kTotalFrames = 25;
  constexpr uint32_t kSlotCount = 4;
  constexpr size_t kFrameBytes = 1920 * 1080;

  // 1. Initialize NvSciBuf pool (4 slots)
  NvSciBufPoolConfig config;
  config.slot_count = kSlotCount;
  config.slot_size = kFrameBytes;
  config.alignment = 4096;

  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  // 2. Initialize NvSciSync engine & Channel Session
  auto sync_engine = std::make_shared<NvSciSyncEngine>(100);
  const uint64_t channel_id = 8888;
  auto session =
      std::make_shared<GpuChannelSession>(channel_id, pool, sync_engine);

  // Register two consumers (Consumer A and Consumer B)
  constexpr uint64_t kConsumerA = 1001;
  constexpr uint64_t kConsumerB = 1002;
  session->RegisterConsumer(kConsumerA);
  session->RegisterConsumer(kConsumerB);

  std::atomic<uint32_t> consumer_a_received{0};
  std::atomic<uint32_t> consumer_b_received{0};

  // 3. Producer streams frames
  for (uint32_t frame = 1; frame <= kTotalFrames; ++frame) {
    // Acquire slot
    int slot = pool->AcquireSlot();
    int retries = 0;
    while (slot < 0 && retries < 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      slot = pool->AcquireSlot();
      retries++;
    }
    ASSERT_GE(slot, 0) << "Buffer pool starved at frame " << frame;

    // Fill GPU buffer
    void* dev_ptr = pool->GetDevicePtr(slot);
    ASSERT_NE(dev_ptr, nullptr);
    const uint8_t fill_val = static_cast<uint8_t>(frame & 0xFF);
#if defined(CYBER_USE_CUDA_IPC)
    ASSERT_EQ(cudaMemset(dev_ptr, fill_val, 1024), cudaSuccess);
#else
    std::memset(dev_ptr, fill_val, 1024);
#endif

    CameraFrameMeta meta;
    meta.frame_id = frame;
    meta.timestamp_ns = frame * 1000000ULL;

    GpuTransportPacket packet;
    {
      GpuLoanedMessage<CameraFrameMeta> loaned(slot, dev_ptr, &meta, session);
      ASSERT_TRUE(loaned.Publish(&packet));
    }

    // Consumer A receives packet and processes
    {
      GpuConstView<CameraFrameMeta> view_a(packet, &meta, kConsumerA, session);
      ASSERT_TRUE(view_a.WaitUntilReady(nullptr));
      uint8_t read_byte = 0;
#if defined(CYBER_USE_CUDA_IPC)
      ASSERT_EQ(cudaMemcpy(&read_byte, view_a.device_ptr(), 1,
                           cudaMemcpyDeviceToHost),
                cudaSuccess);
#else
      read_byte = *static_cast<const uint8_t*>(view_a.device_ptr());
#endif
      EXPECT_EQ(read_byte, fill_val);
      EXPECT_EQ(view_a->frame_id, frame);
      ASSERT_TRUE(view_a.SignalCompletion(nullptr));
      consumer_a_received++;
    }

    // Consumer B receives packet and processes
    {
      GpuConstView<CameraFrameMeta> view_b(packet, &meta, kConsumerB, session);
      ASSERT_TRUE(view_b.WaitUntilReady(nullptr));
      uint8_t read_byte = 0;
#if defined(CYBER_USE_CUDA_IPC)
      ASSERT_EQ(cudaMemcpy(&read_byte, view_b.device_ptr(), 1,
                           cudaMemcpyDeviceToHost),
                cudaSuccess);
#else
      read_byte = *static_cast<const uint8_t*>(view_b.device_ptr());
#endif
      EXPECT_EQ(read_byte, fill_val);
      EXPECT_EQ(view_b->frame_id, frame);
      ASSERT_TRUE(view_b.SignalCompletion(nullptr));
      consumer_b_received++;
    }

    // Both consumers completed; slot must return to FREE immediately
    EXPECT_EQ(pool->GetSlotState(slot), SlotState::FREE);
  }

  EXPECT_EQ(consumer_a_received.load(), kTotalFrames);
  EXPECT_EQ(consumer_b_received.load(), kTotalFrames);

  // All slots in pool should be FREE after pipeline finishes
  for (uint32_t i = 0; i < kSlotCount; ++i) {
    EXPECT_EQ(pool->GetSlotState(i), SlotState::FREE);
  }
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

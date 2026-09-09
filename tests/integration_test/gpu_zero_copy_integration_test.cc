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

#include <chrono>
#include <memory>
#include <vector>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "cyber/transport/nvsci/gpu_channel_session.h"
#include "cyber/transport/nvsci/gpu_loaned_message.h"

namespace apollo {
namespace cyber {
namespace transport {

struct CudaImageHeader {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t frame_id = 0;
  uint64_t timestamp_ns = 0;
};

TEST(GpuZeroCopyIntegrationTest, RealGpuPublishAndReceiveLoop) {
  int device_count = 0;
  cudaError_t err = cudaGetDeviceCount(&device_count);
  ASSERT_EQ(err, cudaSuccess);
  if (device_count == 0) {
    GTEST_SKIP() << "No CUDA device available on host.";
  }

  // 1. Create Producer and Consumer CUDA Streams
  cudaStream_t producer_stream = nullptr;
  cudaStream_t consumer_stream = nullptr;
  ASSERT_EQ(cudaStreamCreateWithFlags(&producer_stream, cudaStreamNonBlocking),
            cudaSuccess);
  ASSERT_EQ(cudaStreamCreateWithFlags(&consumer_stream, cudaStreamNonBlocking),
            cudaSuccess);

  // 2. Configure and allocate GPU memory pool
  constexpr uint32_t kSlotCount = 4;
  constexpr size_t kSlotBytes = 1920 * 1080 * 2;  // ~4.14 MB (YUV422)

  NvSciBufPoolConfig config;
  config.slot_count = kSlotCount;
  config.slot_size = kSlotBytes;
  config.alignment = 4096;

  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());

  // Verify memory allocated is genuine GPU Device Memory
  void* probe_ptr = pool->GetDevicePtr(0);
  ASSERT_NE(probe_ptr, nullptr);
  cudaPointerAttributes attrs{};
  err = cudaPointerGetAttributes(&attrs, probe_ptr);
  EXPECT_EQ(err, cudaSuccess);
#if CUDART_VERSION >= 10000
  EXPECT_EQ(attrs.type, cudaMemoryTypeDevice);
#endif

  // 3. Initialize sync engine & channel session
  auto sync_engine = std::make_shared<NvSciSyncEngine>(2026);
  constexpr uint64_t kChannelId = 9999;
  auto session =
      std::make_shared<GpuChannelSession>(kChannelId, pool, sync_engine);

  constexpr uint64_t kConsumerId = 5001;
  session->RegisterConsumer(kConsumerId);

  // 4. Stream 10 frames end-to-end on GPU with hardware async synchronization
  constexpr uint32_t kFramesToTest = 10;
  for (uint32_t frame = 1; frame <= kFramesToTest; ++frame) {
    int slot = pool->AcquireSlot();
    ASSERT_GE(slot, 0) << "GPU Buffer pool starved at frame " << frame;

    void* dev_ptr = pool->GetDevicePtr(slot);
    ASSERT_NE(dev_ptr, nullptr);

    const uint8_t fill_byte = static_cast<uint8_t>(0xA0 + frame);

    // Producer GPU write: write test pattern directly on GPU stream
    ASSERT_EQ(cudaMemsetAsync(dev_ptr, fill_byte, 1024, producer_stream),
              cudaSuccess);

    CudaImageHeader header;
    header.frame_id = frame;
    header.timestamp_ns = frame * 33333333ULL;  // ~30 fps

    GpuTransportPacket packet;
    {
      // RAII loan: publishes asynchronously on producer_stream
      GpuLoanedMessage<CudaImageHeader> loaned(slot, dev_ptr, &header, session,
                                               producer_stream);
      ASSERT_TRUE(loaned.Publish(&packet));
    }

    // Consumer receives packet and mounts hardware wait on consumer_stream
    {
      GpuConstView<CudaImageHeader> view(packet, &header, kConsumerId, session);
      // Inserts hardware wait on consumer_stream (CPU returns immediately)
      ASSERT_TRUE(view.WaitUntilReady(consumer_stream));

      // Read back 1 byte via consumer_stream to verify GPU data integrity
      uint8_t host_val = 0;
      ASSERT_EQ(cudaMemcpyAsync(&host_val, view.device_ptr(), 1,
                                cudaMemcpyDeviceToHost, consumer_stream),
                cudaSuccess);

      // Synchronize only consumer_stream to verify verification result
      ASSERT_EQ(cudaStreamSynchronize(consumer_stream), cudaSuccess);
      EXPECT_EQ(host_val, fill_byte);
      EXPECT_EQ(view->frame_id, frame);

      // Signal completion on consumer stream
      ASSERT_TRUE(view.SignalCompletion(consumer_stream));
    }

    // Slot must return to FREE
    EXPECT_EQ(pool->GetSlotState(slot), SlotState::FREE);
  }

  // Cleanup streams
  cudaStreamDestroy(producer_stream);
  cudaStreamDestroy(consumer_stream);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

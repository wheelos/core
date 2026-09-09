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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "cyber/cyber.h"
#include "cyber/transport/nvsci/gpu_channel_session.h"
#include "cyber/transport/nvsci/gpu_loaned_message.h"
#include "cyber/transport/nvsci/gpu_reader.h"
#include "cyber/transport/nvsci/gpu_writer.h"

namespace apollo {
namespace cyber {
namespace transport {
namespace {

struct PerfImageMeta {
  uint32_t frame_id = 0;
  uint32_t payload_bytes = 0;
};

struct GpuTestContext {
  cudaStream_t producer_stream = nullptr;
  cudaStream_t consumer_stream = nullptr;
  std::shared_ptr<NvSciBufPool> pool;
  std::shared_ptr<NvSciSyncEngine> sync_engine;
  std::shared_ptr<GpuChannelSession> session;
};

void DestroyGpu(GpuTestContext* context) {
  if (context->producer_stream != nullptr) {
    cudaStreamDestroy(context->producer_stream);
    context->producer_stream = nullptr;
  }
  if (context->consumer_stream != nullptr) {
    cudaStreamDestroy(context->consumer_stream);
    context->consumer_stream = nullptr;
  }
}

bool InitializeGpu(GpuTestContext* context, uint32_t slot_count,
                   size_t slot_size, uint64_t channel_id,
                   uint64_t consumer_id) {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    return false;
  }
  if (cudaStreamCreateWithFlags(&context->producer_stream,
                                cudaStreamNonBlocking) != cudaSuccess ||
      cudaStreamCreateWithFlags(&context->consumer_stream,
                                cudaStreamNonBlocking) != cudaSuccess) {
    DestroyGpu(context);
    return false;
  }

  NvSciBufPoolConfig config;
  config.slot_count = slot_count;
  config.slot_size = slot_size;
  config.alignment = 4096;
  context->pool = std::make_shared<NvSciBufPool>(config);
  if (!context->pool->Initialize()) {
    DestroyGpu(context);
    return false;
  }

  void* device_ptr = context->pool->GetDevicePtr(0);
  cudaPointerAttributes attributes{};
  if (device_ptr == nullptr ||
      cudaPointerGetAttributes(&attributes, device_ptr) != cudaSuccess ||
      attributes.type != cudaMemoryTypeDevice) {
    DestroyGpu(context);
    return false;
  }

  context->sync_engine = std::make_shared<NvSciSyncEngine>(channel_id);
  context->session = std::make_shared<GpuChannelSession>(
      channel_id, context->pool, context->sync_engine);
  context->session->RegisterConsumer(consumer_id);
  return true;
}

int AcquireSlot(const std::shared_ptr<NvSciBufPool>& pool) {
  for (int retry = 0; retry < 1000; ++retry) {
    const int slot = pool->AcquireSlot();
    if (slot >= 0) {
      return slot;
    }
    std::this_thread::yield();
  }
  return -1;
}

struct PerfMatrixCase {
  const char* name;
  uint32_t slot_count;
  size_t slot_size;
  uint32_t frames;
  double min_fps;
};

void RunOnePerfCase(const PerfMatrixCase& test_case, uint64_t channel_id,
                    uint64_t consumer_id) {
  GpuTestContext context;
  ASSERT_TRUE(InitializeGpu(&context, test_case.slot_count, test_case.slot_size,
                           channel_id, consumer_id));

  const auto start = std::chrono::steady_clock::now();
  for (uint32_t frame = 1; frame <= test_case.frames; ++frame) {
    const int slot = AcquireSlot(context.pool);
    ASSERT_GE(slot, 0) << test_case.name << " pool starvation at frame " << frame;
    void* device_ptr = context.pool->GetDevicePtr(slot);
    ASSERT_NE(device_ptr, nullptr);

    PerfImageMeta meta{frame, static_cast<uint32_t>(test_case.slot_size)};
    ASSERT_EQ(cudaMemsetAsync(device_ptr, static_cast<int>(frame & 0xff),
                              test_case.slot_size, context.producer_stream),
              cudaSuccess);

    GpuTransportPacket packet;
    {
      GpuLoanedMessage<PerfImageMeta> message(slot, device_ptr, &meta,
                                              context.session,
                                              context.producer_stream);
      ASSERT_TRUE(message.Publish(&packet));
    }

    GpuConstView<PerfImageMeta> view(packet, &meta, consumer_id, context.session);
    ASSERT_TRUE(view.WaitUntilReady(context.consumer_stream));
    ASSERT_EQ(cudaStreamSynchronize(context.consumer_stream), cudaSuccess);
    ASSERT_TRUE(view.SignalCompletion(context.consumer_stream));
  }

  ASSERT_EQ(cudaStreamSynchronize(context.producer_stream), cudaSuccess);
  const double elapsed_seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  const double fps = static_cast<double>(test_case.frames) / std::max(1e-9, elapsed_seconds);
  std::cout << std::fixed << std::setprecision(2)
            << "\n[perf-matrix] " << test_case.name << ": " << fps << " FPS "
            << "(" << elapsed_seconds << " s)\n";
  EXPECT_GE(fps, test_case.min_fps)
      << test_case.name << " underperformed expected floor";
  DestroyGpu(&context);
}

}  // namespace

TEST(GpuZeroCopyPerfTest, DeviceMemoryAndDataIntegrityStress) {
  GpuTestContext context;
  if (!InitializeGpu(&context, 4, 1U << 20, 7001, 101)) {
    GTEST_SKIP() << "A CUDA device with device-backed pool memory is required.";
  }

  constexpr uint32_t kFrames = 256;
  constexpr size_t kCheckedBytes = 4096;
  std::vector<double> latency_us;
  latency_us.reserve(kFrames);
  for (uint32_t frame = 1; frame <= kFrames; ++frame) {
    const auto frame_start = std::chrono::steady_clock::now();
    const int slot = AcquireSlot(context.pool);
    ASSERT_GE(slot, 0) << "pool starvation at frame " << frame;
    void* device_ptr = context.pool->GetDevicePtr(slot);
    ASSERT_NE(device_ptr, nullptr);

    const uint8_t pattern = static_cast<uint8_t>(frame);
    ASSERT_EQ(cudaMemsetAsync(device_ptr, pattern, kCheckedBytes,
                              context.producer_stream),
              cudaSuccess);

    PerfImageMeta meta{frame, static_cast<uint32_t>(kCheckedBytes)};
    GpuTransportPacket packet;
    {
      GpuLoanedMessage<PerfImageMeta> message(
          slot, device_ptr, &meta, context.session, context.producer_stream);
      ASSERT_TRUE(message.Publish(&packet));
    }

    uint8_t observed = 0;
    {
      GpuConstView<PerfImageMeta> view(packet, &meta, 101, context.session);
      ASSERT_TRUE(view.WaitUntilReady(context.consumer_stream));
      ASSERT_EQ(cudaMemcpyAsync(&observed, view.device_ptr(), sizeof(observed),
                                cudaMemcpyDeviceToHost, context.consumer_stream),
                cudaSuccess);
      ASSERT_EQ(cudaStreamSynchronize(context.consumer_stream), cudaSuccess);
      ASSERT_EQ(observed, pattern) << "data mismatch at frame " << frame;
      ASSERT_EQ(view->frame_id, frame);
      ASSERT_TRUE(view.SignalCompletion(context.consumer_stream));
    }
    latency_us.push_back(std::chrono::duration<double, std::micro>(
                             std::chrono::steady_clock::now() - frame_start)
                             .count());
    ASSERT_EQ(context.pool->GetSlotState(slot), SlotState::FREE);
  }

  std::sort(latency_us.begin(), latency_us.end());
  const auto percentile = [&latency_us](double fraction) {
    const size_t index = static_cast<size_t>(
        fraction * static_cast<double>(latency_us.size() - 1));
    return latency_us[index];
  };
  std::cout << std::fixed << std::setprecision(2)
            << "\nGPU zero-copy end-to-end latency: p50 "
            << percentile(0.50) << " us, p95 " << percentile(0.95)
            << " us, max " << latency_us.back() << " us\n";
  DestroyGpu(&context);
}

TEST(GpuZeroCopyPerfTest, SustainedRingBufferThroughput) {
  GpuTestContext context;
  if (!InitializeGpu(&context, 8, 4U << 20, 7002, 102)) {
    GTEST_SKIP() << "A CUDA device with device-backed pool memory is required.";
  }

  constexpr uint32_t kFrames = 1000;
  constexpr size_t kPayloadBytes = 1U << 20;
  uint8_t* verification = nullptr;
  ASSERT_EQ(cudaMalloc(reinterpret_cast<void**>(&verification), 1), cudaSuccess);
  const auto start = std::chrono::steady_clock::now();

  for (uint32_t frame = 1; frame <= kFrames; ++frame) {
    const int slot = AcquireSlot(context.pool);
    ASSERT_GE(slot, 0) << "pool starvation at frame " << frame;
    void* device_ptr = context.pool->GetDevicePtr(slot);
    ASSERT_EQ(cudaMemsetAsync(device_ptr, static_cast<int>(frame & 0xff),
                              kPayloadBytes, context.producer_stream),
              cudaSuccess);

    PerfImageMeta meta{frame, static_cast<uint32_t>(kPayloadBytes)};
    GpuTransportPacket packet;
    {
      GpuLoanedMessage<PerfImageMeta> message(
          slot, device_ptr, &meta, context.session, context.producer_stream);
      ASSERT_TRUE(message.Publish(&packet));
    }
    GpuConstView<PerfImageMeta> view(packet, &meta, 102, context.session);
    ASSERT_TRUE(view.WaitUntilReady(context.consumer_stream));
    ASSERT_EQ(cudaMemcpyAsync(verification, view.device_ptr(), 1,
                              cudaMemcpyDeviceToDevice, context.consumer_stream),
              cudaSuccess);
    ASSERT_TRUE(view.SignalCompletion(context.consumer_stream));
  }

  ASSERT_EQ(cudaStreamSynchronize(context.producer_stream), cudaSuccess);
  ASSERT_EQ(cudaStreamSynchronize(context.consumer_stream), cudaSuccess);
  ASSERT_EQ(cudaFree(verification), cudaSuccess);
  const double elapsed_seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  ASSERT_GT(elapsed_seconds, 0.0);

  const double frames_per_second = kFrames / elapsed_seconds;
  const double gbps = (static_cast<double>(kFrames) * kPayloadBytes * 8.0) /
                      elapsed_seconds / 1.0e9;
  std::cout << std::fixed << std::setprecision(2)
            << "\nGPU-GPU zero-copy throughput: " << frames_per_second
            << " FPS, " << gbps << " Gb/s, " << elapsed_seconds << " s\n";
  EXPECT_EQ(context.pool->GetSlotState(0), SlotState::FREE);
  DestroyGpu(&context);
}

TEST(GpuZeroCopyPerfTest, FanoutStress) {
  GpuTestContext context;
  if (!InitializeGpu(&context, 8, 1U << 20, 7003, 201)) {
    GTEST_SKIP() << "A CUDA device with device-backed pool memory is required.";
  }
  context.session->RegisterConsumer(202);
  context.session->RegisterConsumer(203);

  constexpr uint32_t kFrames = 300;
  for (uint32_t frame = 1; frame <= kFrames; ++frame) {
    const int slot = AcquireSlot(context.pool);
    ASSERT_GE(slot, 0) << "fanout pool starvation at frame " << frame;
    void* device_ptr = context.pool->GetDevicePtr(slot);
    ASSERT_EQ(cudaMemsetAsync(device_ptr, static_cast<int>(frame & 0xff), 4096,
                              context.producer_stream),
              cudaSuccess);

    PerfImageMeta meta{frame, 4096};
    GpuTransportPacket packet;
    {
      GpuLoanedMessage<PerfImageMeta> message(
          slot, device_ptr, &meta, context.session, context.producer_stream);
      ASSERT_TRUE(message.Publish(&packet));
    }
    for (const uint64_t consumer_id : {uint64_t{201}, uint64_t{202},
                                       uint64_t{203}}) {
      GpuConstView<PerfImageMeta> view(packet, &meta, consumer_id,
                                       context.session);
      ASSERT_TRUE(view.WaitUntilReady(context.consumer_stream));
      ASSERT_TRUE(view.SignalCompletion(context.consumer_stream));
    }
  }

  ASSERT_EQ(cudaStreamSynchronize(context.producer_stream), cudaSuccess);
  ASSERT_EQ(cudaStreamSynchronize(context.consumer_stream), cudaSuccess);
  EXPECT_EQ(context.pool->GetSlotState(0), SlotState::FREE);
  DestroyGpu(&context);
}

TEST(GpuZeroCopyPerfTest, HighLevelApiBackpressureAndPublishRecovery) {
  GpuTestContext context;
  if (!InitializeGpu(&context, 2, 1U << 16, 7004, 204)) {
    GTEST_SKIP() << "A CUDA device with device-backed pool memory is required.";
  }

  PerfImageMeta first_meta{1, 4096};
  PerfImageMeta second_meta{2, 4096};
  GpuTransportPacket first_packet;
  GpuTransportPacket second_packet;
  {
    GpuLoanedMessage<PerfImageMeta> first(&first_meta, context.session,
                                          context.producer_stream);
    ASSERT_TRUE(first.is_valid());
    ASSERT_EQ(cudaMemsetAsync(first.device_ptr(), 0x11, first_meta.payload_bytes,
                              context.producer_stream),
              cudaSuccess);
    ASSERT_TRUE(first.Publish(&first_packet));

    GpuLoanedMessage<PerfImageMeta> second(&second_meta, context.session,
                                           context.producer_stream);
    ASSERT_TRUE(second.is_valid());
    ASSERT_EQ(cudaMemsetAsync(second.device_ptr(), 0x22,
                              second_meta.payload_bytes,
                              context.producer_stream),
              cudaSuccess);
    ASSERT_TRUE(second.Publish(&second_packet));
  }

  // Both slots are held by the consumer. A third loan must fail without
  // corrupting the pool, and must recover after the consumer drains it.
  PerfImageMeta blocked_meta{3, 4096};
  GpuLoanedMessage<PerfImageMeta> blocked(&blocked_meta, context.session,
                                          context.producer_stream);
  EXPECT_FALSE(blocked.is_valid());
  EXPECT_FALSE(blocked.Publish());

  const auto complete = [&](const GpuTransportPacket& packet,
                            const PerfImageMeta& meta) {
    GpuConstView<PerfImageMeta> view(packet, &meta, 204, context.session);
    ASSERT_TRUE(view.WaitUntilReady(context.consumer_stream));
    ASSERT_TRUE(view.SignalCompletion(context.consumer_stream));
  };
  complete(first_packet, first_meta);
  complete(second_packet, second_meta);
  ASSERT_EQ(cudaStreamSynchronize(context.consumer_stream), cudaSuccess);

  PerfImageMeta recovered_meta{4, 4096};
  GpuTransportPacket recovered_packet;
  {
    GpuLoanedMessage<PerfImageMeta> recovered(
        &recovered_meta, context.session, context.producer_stream);
    ASSERT_TRUE(recovered.is_valid());
    ASSERT_TRUE(recovered.Publish(&recovered_packet));
  }
  GpuConstView<PerfImageMeta> recovered_view(
      recovered_packet, &recovered_meta, 204, context.session);
  ASSERT_TRUE(recovered_view.WaitUntilReady(context.consumer_stream));
  ASSERT_TRUE(recovered_view.SignalCompletion(context.consumer_stream));
  ASSERT_EQ(cudaStreamSynchronize(context.consumer_stream), cudaSuccess);

  EXPECT_EQ(context.pool->GetSlotState(0), SlotState::FREE);
  EXPECT_EQ(context.pool->GetSlotState(1), SlotState::FREE);
  DestroyGpu(&context);
}

TEST(GpuZeroCopyPerfTest, SustainedPressureAndIntegrityRecovery) {
  GpuTestContext context;
  if (!InitializeGpu(&context, 4, 1U << 16, 7006, 206)) {
    GTEST_SKIP() << "A CUDA device with device-backed pool memory is required.";
  }

  constexpr uint32_t kRounds = 32;
  constexpr uint32_t kSlots = 4;
  constexpr size_t kCheckedBytes = 4096;
  for (uint32_t round = 0; round < kRounds; ++round) {
    std::array<PerfImageMeta, kSlots> metas{};
    std::array<GpuTransportPacket, kSlots> packets{};
    for (uint32_t index = 0; index < kSlots; ++index) {
      metas[index] = {round * kSlots + index + 1,
                      static_cast<uint32_t>(kCheckedBytes)};
      GpuLoanedMessage<PerfImageMeta> message(
          &metas[index], context.session, context.producer_stream);
      ASSERT_TRUE(message.is_valid());
      ASSERT_EQ(cudaMemsetAsync(
                    message.device_ptr(),
                    static_cast<int>(metas[index].frame_id & 0xff),
                    kCheckedBytes, context.producer_stream),
                cudaSuccess);
      ASSERT_TRUE(message.Publish(&packets[index]));
    }

    PerfImageMeta blocked_meta{round * kSlots + kSlots + 1,
                               static_cast<uint32_t>(kCheckedBytes)};
    GpuLoanedMessage<PerfImageMeta> blocked(
        &blocked_meta, context.session, context.producer_stream);
    EXPECT_FALSE(blocked.is_valid()) << "pool did not apply sustained pressure";

    std::array<uint8_t, kSlots> observed{};
    for (uint32_t index = 0; index < kSlots; ++index) {
      GpuConstView<PerfImageMeta> view(packets[index], &metas[index], 206,
                                       context.session);
      ASSERT_TRUE(view.WaitUntilReady(context.consumer_stream));
      ASSERT_EQ(cudaMemcpyAsync(&observed[index], view.device_ptr(),
                                sizeof(observed[index]),
                                cudaMemcpyDeviceToHost,
                                context.consumer_stream),
                cudaSuccess);
      ASSERT_TRUE(view.SignalCompletion(context.consumer_stream));
    }
    ASSERT_EQ(cudaStreamSynchronize(context.consumer_stream), cudaSuccess);
    for (uint32_t index = 0; index < kSlots; ++index) {
      EXPECT_EQ(observed[index],
                static_cast<uint8_t>(metas[index].frame_id & 0xff))
          << "data mismatch in pressure round " << round << ", slot "
          << index;
    }
    for (uint32_t index = 0; index < kSlots; ++index) {
      EXPECT_EQ(context.pool->GetSlotState(index), SlotState::FREE);
    }
  }

  DestroyGpu(&context);
}

TEST(GpuZeroCopyPerfTest, TimeoutQuarantineAndRecovery) {
  GpuTestContext context;
  if (!InitializeGpu(&context, 1, 1U << 16, 7005, 205)) {
    GTEST_SKIP() << "A CUDA device with device-backed pool memory is required.";
  }

  PerfImageMeta meta{1, 4096};
  GpuTransportPacket packet;
  {
    // A streamless fence remains pending until explicitly completed, which
    // makes the quarantine safety check deterministic.
    GpuLoanedMessage<PerfImageMeta> message(&meta, context.session);
    ASSERT_TRUE(message.is_valid());
    ASSERT_TRUE(message.Publish(&packet));
  }
  EXPECT_EQ(context.pool->GetSlotState(0), SlotState::IN_USE);
  EXPECT_EQ(context.session->ReapHungSlots(0), 1U);
  EXPECT_EQ(context.pool->GetSlotState(0), SlotState::QUARANTINED);
  EXPECT_EQ(context.pool->AcquireSlot(), -1);
  EXPECT_EQ(context.session->RecoverQuarantinedSlots(), 0U);

  context.sync_engine->MarkFenceCompleted(packet.prefence.fence_id);
  EXPECT_EQ(context.session->RecoverQuarantinedSlots(), 1U);
  EXPECT_EQ(context.pool->GetSlotState(0), SlotState::FREE);
  DestroyGpu(&context);
}

TEST(GpuZeroCopyPerfTest, PerformanceMatrix) {
  const std::array<PerfMatrixCase, 3> cases = {
      PerfMatrixCase{"small-slot-64k", 2, 64U * 1024U, 128, 1200.0},
      PerfMatrixCase{"mid-slot-1m", 4, 1U << 20, 256, 800.0},
      PerfMatrixCase{"large-fanout-4m", 8, 4U << 20, 128, 250.0},
  };

  for (const auto& test_case : cases) {
    RunOnePerfCase(test_case, 8000 + test_case.slot_count, 9000 + test_case.slot_count);
  }
}

TEST(GpuZeroCopyPerfTest, ResourceLifetimeLeakageProbe) {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    GTEST_SKIP() << "A CUDA device is required for resource lifetime validation.";
  }

  size_t free_before = 0;
  size_t total_before = 0;
  ASSERT_EQ(cudaMemGetInfo(&free_before, &total_before), cudaSuccess);

  for (int round = 0; round < 24; ++round) {
    GpuTestContext context;
    ASSERT_TRUE(InitializeGpu(&context, 4, 1U << 20, 9001 + round, 10001 + round));
    ASSERT_EQ(cudaMemsetAsync(context.pool->GetDevicePtr(0), 0x5a, 4096,
                              context.producer_stream),
              cudaSuccess);
    ASSERT_EQ(cudaStreamSynchronize(context.producer_stream), cudaSuccess);
    ASSERT_EQ(cudaStreamSynchronize(context.consumer_stream), cudaSuccess);
    DestroyGpu(&context);
  }

  size_t free_after = 0;
  size_t total_after = 0;
  ASSERT_EQ(cudaMemGetInfo(&free_after, &total_after), cudaSuccess);
  EXPECT_GE(free_after, free_before - (16U * 1024U * 1024U));
  EXPECT_EQ(total_after, total_before);
}

TEST(GpuZeroCopyPerfTest, FrameworkWriterReaderEndToEndPressure) {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    GTEST_SKIP() << "A CUDA device is required for framework stress test.";
  }

  apollo::cyber::Init("gpu_perf_test_framework");
  auto writer_node = apollo::cyber::CreateNode("gpu_perf_writer_node");
  auto reader_node = apollo::cyber::CreateNode("gpu_perf_reader_node");
  ASSERT_NE(writer_node, nullptr);
  ASSERT_NE(reader_node, nullptr);

  const std::string channel = "perf/gpu_framework_e2e";
  constexpr uint32_t kSlotCount = 4;
  constexpr uint64_t kSlotSize = 64 * 1024;  // 64 KiB
  constexpr uint32_t kTotalFrames = 200;

  GpuWriterOptions w_opts;
  w_opts.slot_count = kSlotCount;
  w_opts.slot_size = kSlotSize;
  w_opts.backpressure = GpuBackpressurePolicy::BLOCK;

  auto writer = CreateGpuWriter<PerfImageMeta>(writer_node, channel, w_opts);
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(writer->is_ready());

  std::atomic<uint32_t> received_count{0};
  std::atomic<bool> data_corrupted{false};

  GpuReaderOptions r_opts;
  r_opts.consumer_id = 9999;
  r_opts.slot_count = kSlotCount;
  r_opts.slot_size = kSlotSize;

  auto reader = CreateGpuReader<PerfImageMeta>(
      reader_node, channel, r_opts,
      [&](const GpuMsgView<PerfImageMeta>& view) {
        const uint32_t fid = view->frame_id;
        const uint8_t expected_val = static_cast<uint8_t>(fid & 0xff);
        uint8_t observed_val = 0;
        if (cudaMemcpy(&observed_val, view.device_ptr(), sizeof(observed_val),
                       cudaMemcpyDeviceToHost) != cudaSuccess) {
          data_corrupted.store(true);
        } else if (observed_val != expected_val) {
          data_corrupted.store(true);
        }
        received_count.fetch_add(1, std::memory_order_release);
      });
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(reader->is_ready());

  const auto discovery_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((!writer->HasReader() || !reader->HasWriter()) &&
         std::chrono::steady_clock::now() < discovery_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(writer->HasReader());
  ASSERT_TRUE(reader->HasWriter());

  const auto start = std::chrono::steady_clock::now();
  for (uint32_t i = 1; i <= kTotalFrames; ++i) {
    auto loan = writer->Loan();
    ASSERT_TRUE(loan.has_value());
    loan->metadata().frame_id = i;
    loan->metadata().payload_bytes = kSlotSize;
    ASSERT_EQ(cudaMemset(loan->device_ptr(), static_cast<int>(i & 0xff),
                         kSlotSize),
              cudaSuccess);
    ASSERT_TRUE(writer->Publish(std::move(*loan)));
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (received_count.load(std::memory_order_acquire) < kTotalFrames &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const double elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  EXPECT_EQ(received_count.load(), kTotalFrames);
  EXPECT_FALSE(data_corrupted.load());

  std::cout << "\nFramework GpuWriter->GpuReader Stress: " << kTotalFrames
            << " frames processed in " << elapsed << " s ("
            << (kTotalFrames / elapsed) << " FPS)\n";

  reader->Shutdown();
  writer->Shutdown();
  GpuChannelManager::Instance()->Clear();
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

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

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "cyber/cyber.h"
#include "cyber/transport/nvsci/gpu_reader.h"
#include "cyber/transport/nvsci/gpu_writer.h"

namespace apollo {
namespace cyber {
namespace transport {

struct TestFrameMeta {
  uint32_t frame_id = 0;
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint64_t timestamp_ns = 0;
};

class GpuWriterReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    apollo::cyber::Init("gpu_writer_reader_test");
    writer_node_ = apollo::cyber::CreateNode("test_writer_node");
    reader_node_ = apollo::cyber::CreateNode("test_reader_node");

    int count = 0;
    if (cudaGetDeviceCount(&count) == cudaSuccess && count > 0) {
      cudaStreamCreateWithFlags(&writer_stream_, cudaStreamNonBlocking);
      cudaStreamCreateWithFlags(&reader_stream_, cudaStreamNonBlocking);
    }
  }

  void TearDown() override {
    if (writer_stream_) {
      cudaStreamDestroy(writer_stream_);
    }
    if (reader_stream_) {
      cudaStreamDestroy(reader_stream_);
    }
    GpuChannelManager::Instance()->Clear();
    writer_node_ = nullptr;
    reader_node_ = nullptr;
  }

  std::shared_ptr<Node> writer_node_ = nullptr;
  std::shared_ptr<Node> reader_node_ = nullptr;
  cudaStream_t writer_stream_ = nullptr;
  cudaStream_t reader_stream_ = nullptr;
};

TEST_F(GpuWriterReaderTest, EndToEndPublishAndReceive) {
  const std::string channel = "test/gpu_e2e_channel";

  GpuWriterOptions w_opts;
  w_opts.slot_count = 2;
  w_opts.slot_size = 1024;
  w_opts.stream = writer_stream_;
  w_opts.backpressure = GpuBackpressurePolicy::DROP;

  auto writer = CreateGpuWriter<TestFrameMeta>(writer_node_, channel, w_opts);
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(writer->is_ready());

  std::atomic<bool> received{false};
  std::atomic<uint32_t> received_frame{0};

  GpuReaderOptions r_opts;
  r_opts.stream = reader_stream_;
  r_opts.consumer_id = 701;

  auto reader = CreateGpuReader<TestFrameMeta>(
      reader_node_, channel, r_opts,
      [&](const GpuMsgView<TestFrameMeta>& view) {
        EXPECT_NE(view.device_ptr(), nullptr);
        EXPECT_EQ(view->width, 1920);
        EXPECT_EQ(view->height, 1080);
        received_frame.store(view->frame_id);

        if (reader_stream_) {
          uint8_t byte_val = 0;
          cudaMemcpyAsync(&byte_val, view.device_ptr(), 1,
                          cudaMemcpyDeviceToHost, reader_stream_);
          cudaStreamSynchronize(reader_stream_);
          EXPECT_EQ(byte_val, 0xAB);
        }

        received.store(true);
      });

  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(reader->is_ready());

  // Wait for Cyber RT intra discovery to establish control connections
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto registration_writer = reader_node_->CreateWriter<message::RawMessage>(
      channel + "/_gpu_registration");
  ASSERT_NE(registration_writer, nullptr);
  for (int i = 0; i < 50 && !registration_writer->HasReader(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(registration_writer->HasReader());
  auto session = GpuChannelManager::Instance()->GetSession(channel);
  ASSERT_NE(session, nullptr);
  ASSERT_EQ(session->GetConsumerCount(), 1U);
  GpuConsumerUnregister stale_unregister;
  stale_unregister.channel_id = session->channel_id();
  stale_unregister.session_id = session->session_id() + 1;
  stale_unregister.consumer_id = r_opts.consumer_id;
  ASSERT_TRUE(registration_writer->Write(std::make_shared<message::RawMessage>(
      EncodeGpuConsumerUnregister(stale_unregister))));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(session->GetConsumerCount(), 1U);

  // 1. Loan a slot
  auto loan = writer->Loan();
  ASSERT_TRUE(loan.has_value());
  EXPECT_TRUE(loan->is_valid());
  EXPECT_NE(loan->device_ptr(), nullptr);

  loan->metadata().frame_id = 99;
  loan->metadata().width = 1920;
  loan->metadata().height = 1080;
  loan->metadata().timestamp_ns = 123456789;

  if (writer_stream_) {
    cudaMemsetAsync(loan->device_ptr(), 0xAB, 1024, writer_stream_);
  }

  // 2. Publish
  EXPECT_TRUE(writer->Publish(std::move(*loan)));

  // 3. Wait for reader callback
  for (int i = 0; i < 50 && !received.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(received.load());
  EXPECT_EQ(received_frame.load(), 99);

  // 4. Check that slot was returned to FREE after callback finished and ACK was
  // processed
  EXPECT_EQ(session->pool()->GetSlotState(0), SlotState::FREE);
}

TEST_F(GpuWriterReaderTest, BackpressureDropPolicy) {
  const std::string channel = "test/gpu_backpressure_channel";

  GpuWriterOptions w_opts;
  w_opts.slot_count = 1;
  w_opts.slot_size = 1024;
  w_opts.backpressure = GpuBackpressurePolicy::DROP;

  auto writer = CreateGpuWriter<TestFrameMeta>(writer_node_, channel, w_opts);
  ASSERT_NE(writer, nullptr);

  auto loan1 = writer->Loan();
  ASSERT_TRUE(loan1.has_value());

  // Second loan must fail with DROP policy since slot_count = 1
  auto loan2 = writer->Loan();
  EXPECT_FALSE(loan2.has_value());

  // Dropping loan1 without publishing cancels and frees the slot
  loan1.reset();

  auto loan3 = writer->Loan();
  EXPECT_TRUE(loan3.has_value());
  EXPECT_FALSE(writer->Publish(std::move(*loan3)));
}

TEST_F(GpuWriterReaderTest, OneWriterFansOutToMultipleReadersOnSameNode) {
  const std::string channel = "test/gpu_multi_reader_channel";
  cudaStream_t second_reader_stream = nullptr;
  if (reader_stream_) {
    ASSERT_EQ(
        cudaStreamCreateWithFlags(&second_reader_stream, cudaStreamNonBlocking),
        cudaSuccess);
  }

  GpuWriterOptions writer_options;
  writer_options.slot_count = 2;
  writer_options.slot_size = 1024;
  writer_options.stream = writer_stream_;
  auto writer =
      CreateGpuWriter<TestFrameMeta>(writer_node_, channel, writer_options);
  ASSERT_NE(writer, nullptr);

  std::atomic<uint32_t> first_count{0};
  std::atomic<uint32_t> second_count{0};
  std::atomic<bool> release_callbacks{false};
  GpuReaderOptions first_options;
  first_options.stream = reader_stream_;
  GpuReaderOptions second_options;
  second_options.stream = second_reader_stream;

  auto first_reader = CreateGpuReader<TestFrameMeta>(
      reader_node_, channel, first_options, [&](GpuMsgView<TestFrameMeta>&) {
        first_count.fetch_add(1);
        while (!release_callbacks.load()) {
          std::this_thread::yield();
        }
      });
  auto second_reader = CreateGpuReader<TestFrameMeta>(
      reader_node_, channel, second_options, [&](GpuMsgView<TestFrameMeta>&) {
        second_count.fetch_add(1);
        while (!release_callbacks.load()) {
          std::this_thread::yield();
        }
      });
  ASSERT_NE(first_reader, nullptr);
  ASSERT_NE(second_reader, nullptr);
  EXPECT_NE(first_reader->consumer_id(), second_reader->consumer_id());

  auto session = GpuChannelManager::Instance()->GetSession(channel);
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->GetConsumerCount(), 2U);

  auto first_loan = writer->Loan();
  ASSERT_TRUE(first_loan.has_value());
  first_loan->metadata().frame_id = 1;
  ASSERT_TRUE(writer->Publish(std::move(*first_loan)));
  for (int i = 0;
       i < 100 && (first_count.load() != 1 || second_count.load() != 1); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(first_count.load(), 1U);
  EXPECT_EQ(second_count.load(), 1U);
  release_callbacks.store(true);

  first_reader.reset();
  EXPECT_EQ(session->GetConsumerCount(), 1U);
  auto second_loan = writer->Loan();
  ASSERT_TRUE(second_loan.has_value());
  second_loan->metadata().frame_id = 2;
  ASSERT_TRUE(writer->Publish(std::move(*second_loan)));
  for (int i = 0; i < 100 && second_count.load() != 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(first_count.load(), 1U);
  EXPECT_EQ(second_count.load(), 2U);

  second_reader.reset();
  if (second_reader_stream) {
    cudaStreamDestroy(second_reader_stream);
  }
}

TEST_F(GpuWriterReaderTest, RejectsSecondWriterForSameChannel) {
  const std::string channel = "test/gpu_single_writer_channel";
  GpuWriterOptions options;
  options.slot_count = 2;
  options.slot_size = 1024;
  options.stream = writer_stream_;

  auto first = CreateGpuWriter<TestFrameMeta>(writer_node_, channel, options);
  ASSERT_NE(first, nullptr);
  const auto original_session =
      GpuChannelManager::Instance()->GetSession(channel);
  ASSERT_NE(original_session, nullptr);

  auto second = CreateGpuWriter<TestFrameMeta>(writer_node_, channel, options);
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(GpuChannelManager::Instance()->GetSession(channel),
            original_session);
  EXPECT_TRUE(first->is_ready());
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

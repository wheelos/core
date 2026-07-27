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

#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"

#include "cyber/cyber.h"
#include "cyber/examples/record_play/record_play.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace record_play {
namespace {

using WriterMap =
    std::unordered_map<std::string, std::shared_ptr<Writer<transport::PodMessage>>>;

struct ChannelRuntimeState {
  std::size_t next_index = 0;
  std::size_t mismatches = 0;
  std::size_t duplicates = 0;
};

class RecordPlayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(LoadRecordPlayItems(kDefaultRecordPath, /*max_per_channel=*/16,
                                    &items_));
    schedule_ = ExpandRecordPlaySchedule(items_, /*repeat=*/2);
    expected_by_channel_.clear();
    for (const auto& item : schedule_) {
      expected_by_channel_[item.channel_name].push_back(item);
    }
  }

  RecordPlayItems items_;
  RecordPlaySchedule schedule_;
  ChannelItems expected_by_channel_;
};

TEST_F(RecordPlayTest, RoundTripBaseline) {
  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<std::string, ChannelRuntimeState> runtime;
  RecordPlayStats stats;
  const auto channels = std::vector<std::string>{
      kImageFront6mm, kImageFront12mm, kPointCloud64};

  std::vector<std::shared_ptr<Node>> reader_nodes;
  std::vector<std::shared_ptr<Reader<transport::PodMessage>>> readers;
  for (const auto& channel : channels) {
    auto attr = MakePodRoleAttr(channel);
    attr.mutable_qos_profile()->set_depth(
        static_cast<uint32_t>(expected_by_channel_[channel].size() * 2 + 8));
    auto node = CreateNode(MakeNodeName("record_play_reader", channel));
    auto reader = node->CreateReader<transport::PodMessage>(
        attr, [&, channel](const std::shared_ptr<transport::PodMessage>& msg) {
          std::lock_guard<std::mutex> lock(mutex);
          auto& expected = expected_by_channel_[channel];
          auto& state = runtime[channel];
          if (state.next_index >= expected.size()) {
            ++state.duplicates;
            ++stats.duplicates;
            return;
          }
          const auto& item = expected[state.next_index];
          if (!ValidateChunk(*msg, item)) {
            ++state.mismatches;
            ++stats.mismatches;
          } else {
            ++stats.total_messages;
            stats.total_bytes += item.payload.size() +
                                 sizeof(transport::PodChunkHeader);
          }
          ++state.next_index;
          cv.notify_one();
        });
    ASSERT_NE(reader, nullptr);
    reader_nodes.emplace_back(std::move(node));
    readers.emplace_back(std::move(reader));
  }

  std::vector<std::shared_ptr<Node>> writer_nodes;
  std::vector<std::shared_ptr<Writer<transport::PodMessage>>> writers;
  for (const auto& channel : channels) {
    auto attr = MakePodRoleAttr(channel);
    attr.mutable_qos_profile()->set_depth(
        static_cast<uint32_t>(expected_by_channel_[channel].size() * 2 + 8));
    auto node = CreateNode(MakeNodeName("record_play_writer", channel));
    auto writer = node->CreateWriter<transport::PodMessage>(attr);
    ASSERT_NE(writer, nullptr);
    writer_nodes.emplace_back(std::move(node));
    writers.emplace_back(std::move(writer));
  }

  ASSERT_TRUE(WaitFor(
      [&]() {
        for (const auto& writer : writers) {
          if (!writer->HasReader()) {
            return false;
          }
        }
        for (const auto& reader : readers) {
          if (!reader->HasWriter()) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(10)));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto start = std::chrono::steady_clock::now();
  WriterMap writer_map = {
      {kImageFront6mm, writers[0]},
      {kImageFront12mm, writers[1]},
      {kPointCloud64, writers[2]},
  };
  ASSERT_TRUE(PublishSchedule(schedule_, writer_map));

  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return stats.total_messages == schedule_.size();
      },
      std::chrono::seconds(30)));
  const auto end = std::chrono::steady_clock::now();

  const double elapsed_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start)
          .count();
  stats.throughput_mb_s =
      (static_cast<double>(stats.total_bytes) / (1024.0 * 1024.0)) /
      std::max(0.001, elapsed_s);
  stats.throughput_msg_s = static_cast<double>(stats.total_messages) /
                           std::max(0.001, elapsed_s);

  EXPECT_EQ(stats.mismatches, 0U);
  EXPECT_EQ(stats.duplicates, 0U);
  EXPECT_GT(stats.throughput_mb_s, 1.0);
  EXPECT_GT(stats.throughput_msg_s, 1.0);
}

TEST_F(RecordPlayTest, BurstStressNoLoss) {
  const auto stress_schedule = ExpandRecordPlaySchedule(items_, /*repeat=*/3);
  ChannelItems stress_expected_by_channel;
  for (const auto& item : stress_schedule) {
    stress_expected_by_channel[item.channel_name].push_back(item);
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<std::string, ChannelRuntimeState> runtime;
  RecordPlayStats stats;
  const auto channels = std::vector<std::string>{
      kImageFront6mm, kImageFront12mm, kPointCloud64};

  std::vector<std::shared_ptr<Node>> reader_nodes;
  std::vector<std::shared_ptr<Reader<transport::PodMessage>>> readers;
  for (const auto& channel : channels) {
    auto attr = MakePodRoleAttr(channel);
    attr.mutable_qos_profile()->set_depth(static_cast<uint32_t>(
        stress_expected_by_channel[channel].size() * 2 + 8));
    auto node = CreateNode(MakeNodeName("record_play_burst_reader", channel));
    auto reader = node->CreateReader<transport::PodMessage>(
        attr, [&, channel](const std::shared_ptr<transport::PodMessage>& msg) {
          std::lock_guard<std::mutex> lock(mutex);
          auto& expected = stress_expected_by_channel[channel];
          auto& state = runtime[channel];
          if (state.next_index >= expected.size()) {
            ++state.duplicates;
            ++stats.duplicates;
            return;
          }
          const auto& item = expected[state.next_index];
          if (!ValidateChunk(*msg, item)) {
            ++state.mismatches;
            ++stats.mismatches;
          } else {
            ++stats.total_messages;
            stats.total_bytes += item.payload.size() +
                                 sizeof(transport::PodChunkHeader);
          }
          ++state.next_index;
          cv.notify_one();
        });
    ASSERT_NE(reader, nullptr);
    reader_nodes.emplace_back(std::move(node));
    readers.emplace_back(std::move(reader));
  }

  std::vector<std::shared_ptr<Node>> writer_nodes;
  std::vector<std::shared_ptr<Writer<transport::PodMessage>>> writers;
  for (const auto& channel : channels) {
    auto attr = MakePodRoleAttr(channel);
    attr.mutable_qos_profile()->set_depth(static_cast<uint32_t>(
        stress_expected_by_channel[channel].size() * 2 + 8));
    auto node = CreateNode(MakeNodeName("record_play_burst_writer", channel));
    auto writer = node->CreateWriter<transport::PodMessage>(attr);
    ASSERT_NE(writer, nullptr);
    writer_nodes.emplace_back(std::move(node));
    writers.emplace_back(std::move(writer));
  }

  ASSERT_TRUE(WaitFor(
      [&]() {
        for (const auto& writer : writers) {
          if (!writer->HasReader()) {
            return false;
          }
        }
        for (const auto& reader : readers) {
          if (!reader->HasWriter()) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(10)));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto start = std::chrono::steady_clock::now();
  WriterMap writer_map = {
      {kImageFront6mm, writers[0]},
      {kImageFront12mm, writers[1]},
      {kPointCloud64, writers[2]},
  };
  ASSERT_TRUE(PublishSchedule(stress_schedule, writer_map,
                              std::chrono::milliseconds(1)));

  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return stats.total_messages == stress_schedule.size();
      },
      std::chrono::seconds(120)));
  const auto end = std::chrono::steady_clock::now();

  const double elapsed_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start)
          .count();
  stats.throughput_mb_s =
      (static_cast<double>(stats.total_bytes) / (1024.0 * 1024.0)) /
      std::max(0.001, elapsed_s);
  stats.throughput_msg_s = static_cast<double>(stats.total_messages) /
                           std::max(0.001, elapsed_s);

  EXPECT_EQ(stats.mismatches, 0U);
  EXPECT_EQ(stats.duplicates, 0U);
  EXPECT_GT(stats.throughput_mb_s, 1.0);
  EXPECT_GT(stats.throughput_msg_s, 1.0);
}

}  // namespace
}  // namespace record_play
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  apollo::cyber::Init(argv[0]);
  const int ret = RUN_ALL_TESTS();
  apollo::cyber::AsyncShutdown();
  apollo::cyber::WaitForShutdown();
  return ret;
}

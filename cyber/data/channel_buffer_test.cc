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

#include "cyber/data/channel_buffer.h"

#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "cyber/common/util.h"

namespace apollo {
namespace cyber {
namespace data {

auto channel0 = common::Hash("/channel0");

TEST(ChannelBufferTest, Fetch) {
  auto cache_buffer = new CacheBuffer<std::shared_ptr<int>>(2);
  auto buffer = std::make_shared<ChannelBuffer<int>>(channel0, cache_buffer);
  std::shared_ptr<int> msg;
  uint64_t index = 0;
  EXPECT_FALSE(buffer->Fetch(&index, msg));
  buffer->Buffer()->Fill(std::make_shared<int>(1));
  EXPECT_TRUE(buffer->Fetch(&index, msg));
  EXPECT_EQ(1, *msg);
  EXPECT_EQ(1, index);
  index++;
  EXPECT_FALSE(buffer->Fetch(&index, msg));
  buffer->Buffer()->Fill(std::make_shared<int>(2));
  buffer->Buffer()->Fill(std::make_shared<int>(3));
  buffer->Buffer()->Fill(std::make_shared<int>(4));
  EXPECT_TRUE(buffer->Fetch(&index, msg));
  EXPECT_EQ(4, *msg);
  EXPECT_EQ(4, index);
  index++;
  EXPECT_FALSE(buffer->Fetch(&index, msg));
  EXPECT_EQ(4, *msg);
}

TEST(ChannelBufferTest, FirstFetchAndOverflowBothSkipToLatest) {
  auto buffer = std::make_shared<ChannelBuffer<int>>(
      channel0, new CacheBuffer<std::shared_ptr<int>>(3));
  for (int value = 1; value <= 3; ++value) {
    buffer->Buffer()->Fill(std::make_shared<int>(value));
  }
  uint64_t index = 0;
  std::shared_ptr<int> message;
  ASSERT_TRUE(buffer->Fetch(&index, message));
  EXPECT_EQ(index, 3);
  ASSERT_NE(message, nullptr);
  EXPECT_EQ(*message, 3);

  ++index;
  for (int value = 4; value <= 7; ++value) {
    buffer->Buffer()->Fill(std::make_shared<int>(value));
  }
  ASSERT_TRUE(buffer->Fetch(&index, message));
  EXPECT_EQ(index, 7);
  EXPECT_EQ(*message, 7);
  ++index;
  EXPECT_FALSE(buffer->Fetch(&index, message));
}

TEST(ChannelBufferTest, Latest) {
  auto cache_buffer = new CacheBuffer<std::shared_ptr<int>>(10);
  auto buffer = std::make_shared<ChannelBuffer<int>>(channel0, cache_buffer);
  std::shared_ptr<int> msg;
  EXPECT_FALSE(buffer->Latest(msg));

  buffer->Buffer()->Fill(std::make_shared<int>(1));
  EXPECT_TRUE(buffer->Latest(msg));
  EXPECT_EQ(1, *msg);
  EXPECT_TRUE(buffer->Latest(msg));
  EXPECT_EQ(1, *msg);

  buffer->Buffer()->Fill(std::make_shared<int>(2));
  EXPECT_TRUE(buffer->Latest(msg));
  EXPECT_EQ(2, *msg);
}

TEST(ChannelBufferTest, FetchMulti) {
  auto cache_buffer = new CacheBuffer<std::shared_ptr<int>>(2);
  auto buffer = std::make_shared<ChannelBuffer<int>>(channel0, cache_buffer);
  std::vector<std::shared_ptr<int>> vector;
  EXPECT_FALSE(buffer->FetchMulti(1, &vector));
  buffer->Buffer()->Fill(std::make_shared<int>(1));
  EXPECT_TRUE(buffer->FetchMulti(1, &vector));
  EXPECT_EQ(1, vector.size());
  EXPECT_EQ(1, *vector[0]);

  vector.clear();
  buffer->Buffer()->Fill(std::make_shared<int>(2));
  EXPECT_TRUE(buffer->FetchMulti(1, &vector));
  EXPECT_EQ(1, vector.size());
  EXPECT_EQ(2, *vector[0]);

  vector.clear();
  EXPECT_TRUE(buffer->FetchMulti(2, &vector));
  EXPECT_EQ(2, vector.size());
  EXPECT_EQ(1, *vector[0]);
  EXPECT_EQ(2, *vector[1]);

  vector.clear();
  EXPECT_TRUE(buffer->FetchMulti(3, &vector));
  EXPECT_EQ(2, vector.size());
  EXPECT_EQ(1, *vector[0]);
  EXPECT_EQ(2, *vector[1]);
}

TEST(ChannelBufferTest, ConsumerMetricsFollowUnreadCursor) {
  metrics::Registry::Instance().Configure(metrics::Mode::Basic);
  auto metric = metrics::Registry::Instance().RegisterConsumer("/queue-test",
                                                               "slow-reader");
  ASSERT_TRUE(metric);
  ChannelBuffer<int> buffer(channel0, new CacheBuffer<std::shared_ptr<int>>(3));
  buffer.Buffer()->AttachMetrics(metric);
  uint64_t index = 0;
  std::shared_ptr<int> msg;
  for (int i = 1; i <= 3; ++i) {
    buffer.Buffer()->Fill(std::make_shared<int>(i));
  }
  EXPECT_EQ(metric->Snapshot().queue_depth, 1);
  EXPECT_EQ(metric->Snapshot().drop_count[2], 2);
  ASSERT_TRUE(buffer.Fetch(&index, msg));
  EXPECT_EQ(*msg, 3);
  ++index;
  EXPECT_EQ(metric->Snapshot().queue_depth, 0);

  buffer.Buffer()->Fill(std::make_shared<int>(4));
  buffer.Buffer()->Fill(std::make_shared<int>(5));
  ASSERT_TRUE(buffer.Fetch(&index, msg));
  EXPECT_EQ(*msg, 4);
  ++index;
  for (int i = 6; i <= 9; ++i) {
    buffer.Buffer()->Fill(std::make_shared<int>(i));
  }
  EXPECT_EQ(metric->Snapshot().queue_depth, 3);
  EXPECT_EQ(metric->Snapshot().drop_count[0], 2);
  ASSERT_TRUE(buffer.Fetch(&index, msg));
  EXPECT_EQ(*msg, 9);
  EXPECT_EQ(metric->Snapshot().drop_count[1], 2);
  EXPECT_EQ(metric->Snapshot().queue_depth, 0);
  EXPECT_EQ(metric->Snapshot().enqueue_count, 9);
  EXPECT_EQ(metric->Snapshot().dequeue_count, 3);
  EXPECT_EQ(metric->Snapshot().queue_high_watermark, 3);
  EXPECT_EQ(metric->Snapshot().queue_latency.count, 3);
  metrics::Registry::Instance().Configure(metrics::Mode::Off);
}

TEST(ChannelBufferTest, OverwritingConsumedHistoryIsNotADrop) {
  metrics::Registry::Instance().Configure(metrics::Mode::Basic);
  auto metric = metrics::Registry::Instance().RegisterConsumer("/queue-history",
                                                               "reader");
  ASSERT_TRUE(metric);
  ChannelBuffer<int> buffer(channel0, new CacheBuffer<std::shared_ptr<int>>(2));
  buffer.Buffer()->AttachMetrics(metric);
  uint64_t index = 0;
  std::shared_ptr<int> message;
  for (int value = 1; value <= 4; ++value) {
    buffer.Buffer()->Fill(std::make_shared<int>(value));
    ASSERT_TRUE(buffer.Fetch(&index, message));
    EXPECT_EQ(*message, value);
    ++index;
    EXPECT_EQ(metric->Snapshot().queue_depth, 0);
  }
  const auto snapshot = metric->Snapshot();
  EXPECT_EQ(snapshot.enqueue_count, 4);
  EXPECT_EQ(snapshot.dequeue_count, 4);
  for (auto count : snapshot.drop_count) EXPECT_EQ(count, 0);
  metrics::Registry::Instance().Configure(metrics::Mode::Off);
}

TEST(ChannelBufferTest, ShutdownCountsOnlyStillReadableMessages) {
  metrics::Registry::Instance().Configure(metrics::Mode::Basic);
  auto metric = metrics::Registry::Instance().RegisterConsumer(
      "/shutdown-discard", "reader");
  ASSERT_TRUE(metric);
  {
    ChannelBuffer<int> buffer(channel0,
                              new CacheBuffer<std::shared_ptr<int>>(3));
    buffer.Buffer()->AttachMetrics(metric);
    uint64_t index = 0;
    std::shared_ptr<int> message;
    buffer.Buffer()->Fill(std::make_shared<int>(1));
    buffer.Buffer()->Fill(std::make_shared<int>(2));
    ASSERT_TRUE(buffer.Fetch(&index, message));
    EXPECT_EQ(*message, 2);
    ++index;
    buffer.Buffer()->Fill(std::make_shared<int>(3));
    buffer.Buffer()->Fill(std::make_shared<int>(4));
    EXPECT_FALSE(metric->Snapshot().shutdown_discard_count);
    EXPECT_NE(metrics::Registry::Instance().Snapshot().ToJson().find(
                  "\"shutdown_discard\":null"),
              std::string::npos);
  }
  const auto snapshot = metric->Snapshot();
  ASSERT_TRUE(snapshot.shutdown_discard_count);
  EXPECT_EQ(*snapshot.shutdown_discard_count, 2);
  EXPECT_EQ(snapshot.queue_depth, 0);
  EXPECT_NE(metrics::Registry::Instance().Snapshot().ToJson().find(
                "\"shutdown_discard\":2"),
            std::string::npos);
  metrics::Registry::Instance().Configure(metrics::Mode::Off);
}

TEST(ChannelBufferTest, ShutdownWithEmptyQueueReportsZero) {
  metrics::Registry::Instance().Configure(metrics::Mode::Basic);
  auto metric = metrics::Registry::Instance().RegisterConsumer(
      "/shutdown-empty", "reader");
  ASSERT_TRUE(metric);
  {
    CacheBuffer<std::shared_ptr<int>> buffer(2);
    buffer.AttachMetrics(metric);
  }
  const auto snapshot = metric->Snapshot();
  ASSERT_TRUE(snapshot.shutdown_discard_count);
  EXPECT_EQ(*snapshot.shutdown_discard_count, 0);
  metrics::Registry::Instance().Configure(metrics::Mode::Off);
}

TEST(ChannelBufferTest, ShutdownBeforeFirstFetchCountsOnlyLatestEntry) {
  metrics::Registry::Instance().Configure(metrics::Mode::Basic);
  auto metric = metrics::Registry::Instance().RegisterConsumer(
      "/shutdown-initial", "reader");
  ASSERT_TRUE(metric);
  {
    CacheBuffer<std::shared_ptr<int>> buffer(3);
    buffer.AttachMetrics(metric);
    for (int value = 1; value <= 4; ++value) {
      buffer.Fill(std::make_shared<int>(value));
    }
    EXPECT_EQ(metric->Snapshot().drop_count[2], 3);
  }
  const auto snapshot = metric->Snapshot();
  ASSERT_TRUE(snapshot.shutdown_discard_count);
  EXPECT_EQ(*snapshot.shutdown_discard_count, 1);
  EXPECT_EQ(snapshot.queue_depth, 0);
  metrics::Registry::Instance().Configure(metrics::Mode::Off);
}

TEST(ChannelBufferTest, CopyDoesNotDoubleCountShutdownDiscard) {
  metrics::Registry::Instance().Configure(metrics::Mode::Basic);
  auto metric = metrics::Registry::Instance().RegisterConsumer("/shutdown-copy",
                                                               "reader");
  ASSERT_TRUE(metric);
  {
    CacheBuffer<std::shared_ptr<int>> original(2);
    original.AttachMetrics(metric);
    original.Fill(std::make_shared<int>(1));
    { CacheBuffer<std::shared_ptr<int>> copy(original); }
    EXPECT_FALSE(metric->Snapshot().shutdown_discard_count);
  }
  ASSERT_TRUE(metric->Snapshot().shutdown_discard_count);
  EXPECT_EQ(*metric->Snapshot().shutdown_discard_count, 1);
  metrics::Registry::Instance().Configure(metrics::Mode::Off);
}

}  // namespace data
}  // namespace cyber
}  // namespace apollo

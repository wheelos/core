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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cyber/cyber.h"
#include "cyber/init.h"
#include "examples/record_play/record_play.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace record_play {

struct RecordPlayOptions {
  std::string record_path = kDefaultRecordPath;
  std::string mode = "roundtrip";
  std::size_t max_per_channel = 64;
  std::size_t repeat = 1;
  bool wait_for_reader = true;
  std::size_t publish_interval_ms = 0;
  std::size_t hold_seconds = 0;
};

RecordPlayOptions ParseOptions(int argc, char** argv) {
  RecordPlayOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto eq = arg.find('=');
    const auto key = arg.substr(0, eq);
    const auto value = eq == std::string::npos ? std::string() : arg.substr(eq + 1);
    if (key == "--record" && !value.empty()) {
      options.record_path = value;
    } else if (key == "--mode" && !value.empty()) {
      options.mode = value;
    } else if (key == "--max_per_channel" && !value.empty()) {
      options.max_per_channel = static_cast<std::size_t>(std::stoul(value));
    } else if (key == "--repeat" && !value.empty()) {
      options.repeat = static_cast<std::size_t>(std::stoul(value));
    } else if (key == "--wait_for_reader" && !value.empty()) {
      options.wait_for_reader = value != "0" && value != "false" &&
                                value != "FALSE";
    } else if (key == "--publish_interval_ms" && !value.empty()) {
      options.publish_interval_ms = static_cast<std::size_t>(std::stoul(value));
    } else if (key == "--hold_seconds" && !value.empty()) {
      options.hold_seconds = static_cast<std::size_t>(std::stoul(value));
    }
  }
  return options;
}

using WriterPtr = std::shared_ptr<Writer<transport::PodMessage>>;
using ReaderPtr = std::shared_ptr<Reader<transport::PodMessage>>;

std::unordered_map<std::string, WriterPtr> CreateWriters(
    const std::vector<std::string>& channels,
    std::vector<std::shared_ptr<Node>>* node_store) {
  std::unordered_map<std::string, WriterPtr> writers;
  for (const auto& channel : channels) {
    auto node =
        apollo::cyber::CreateNode(MakeNodeName("record_play_pub", channel));
    auto writer = node->CreateWriter<transport::PodMessage>(channel);
    if (writer == nullptr) {
      AERROR << "failed to create writer for " << channel;
      continue;
    }
    if (node_store != nullptr) {
      node_store->emplace_back(std::move(node));
    }
    writers.emplace(channel, std::move(writer));
  }
  return writers;
}

std::unordered_map<std::string, ReaderPtr> CreateReaders(
    const std::vector<std::string>& channels,
    const std::shared_ptr<std::unordered_map<std::string, RecordPlayItems>>&
        expected_by_channel,
    std::mutex* mutex, std::condition_variable* cv,
    std::unordered_map<std::string, std::size_t>* next_index,
    RecordPlayStats* stats, std::vector<std::shared_ptr<Node>>* node_store) {
  std::unordered_map<std::string, ReaderPtr> readers;
  for (const auto& channel : channels) {
    auto node =
        apollo::cyber::CreateNode(MakeNodeName("record_play_sub", channel));
    auto reader = node->CreateReader<transport::PodMessage>(
        channel,
        [=](const std::shared_ptr<transport::PodMessage>& msg) {
          std::lock_guard<std::mutex> lock(*mutex);
          const auto expected_it = expected_by_channel->find(channel);
          if (expected_it == expected_by_channel->end()) {
            ++stats->mismatches;
            return;
          }
          auto& index = (*next_index)[channel];
          if (index >= expected_it->second.size()) {
            ++stats->duplicates;
            return;
          }
          const auto& expected = expected_it->second[index];
          if (!ValidateChunk(*msg, expected)) {
            ++stats->mismatches;
          } else {
            ++stats->total_messages;
            stats->total_bytes += expected.payload.size() +
                                  sizeof(transport::PodChunkHeader);
          }
          ++index;
          cv->notify_one();
        });
    if (reader == nullptr) {
      AERROR << "failed to create reader for " << channel;
      continue;
    }
    if (node_store != nullptr) {
      node_store->emplace_back(std::move(node));
    }
    readers.emplace(channel, std::move(reader));
  }
  return readers;
}

int RunRoundTrip(const RecordPlayItems& base_items,
                 const RecordPlaySchedule& schedule) {
  const auto channels = std::vector<std::string>{
      kImageFront6mm, kImageFront12mm, kPointCloud64};
  auto expected_by_channel =
      std::make_shared<std::unordered_map<std::string, RecordPlayItems>>();
  for (const auto& item : schedule) {
    (*expected_by_channel)[item.channel_name].push_back(item);
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<std::string, std::size_t> next_index;
  RecordPlayStats stats;

  std::vector<std::shared_ptr<Node>> reader_nodes;
  reader_nodes.reserve(channels.size());
  std::unordered_map<std::string, ReaderPtr> readers = CreateReaders(
      channels, expected_by_channel, &mutex, &cv, &next_index, &stats,
      &reader_nodes);
  if (readers.size() != channels.size()) {
    return 1;
  }

  std::vector<std::shared_ptr<Node>> writer_nodes;
  writer_nodes.reserve(channels.size());
  auto writers = CreateWriters(channels, &writer_nodes);
  if (writers.size() != channels.size()) {
    return 1;
  }

  if (!WaitFor(
          [&]() {
            for (const auto& [_, reader] : readers) {
              if (!reader->HasWriter()) {
                return false;
              }
            }
            return true;
          },
          std::chrono::seconds(30))) {
    AERROR << "timed out waiting for topology discovery";
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto start = std::chrono::steady_clock::now();
  if (!PublishSchedule(schedule, writers)) {
    return 1;
  }

  const auto total_expected = schedule.size();
  if (!WaitFor(
          [&]() {
            std::lock_guard<std::mutex> lock(mutex);
            return stats.total_messages == total_expected;
          },
          std::chrono::seconds(120))) {
    AERROR << "timed out waiting for replay messages";
    return 1;
  }
  const auto end = std::chrono::steady_clock::now();

  const double elapsed_s =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start)
          .count();
  const double mb_s = (static_cast<double>(stats.total_bytes) / (1024.0 * 1024.0)) /
                      std::max(0.001, elapsed_s);
  const double msg_s = static_cast<double>(stats.total_messages) /
                       std::max(0.001, elapsed_s);

  AINFO << "record_play roundtrip baseline: messages=" << stats.total_messages
        << " bytes=" << stats.total_bytes << " throughput_MBps=" << mb_s
        << " throughput_msgps=" << msg_s;

  if (stats.mismatches != 0 || stats.duplicates != 0) {
    AERROR << "mismatches=" << stats.mismatches
           << " duplicates=" << stats.duplicates;
    return 1;
  }

  std::cout << "record_play roundtrip OK: " << ChannelSummary(base_items)
            << ", replayed=" << stats.total_messages
            << ", throughput_MBps=" << mb_s
            << ", throughput_msgps=" << msg_s << std::endl;
  return (mb_s >= 1.0 && msg_s >= 1.0) ? 0 : 1;
}

int RunPublishOnly(const RecordPlaySchedule& schedule, bool wait_for_reader,
                   std::size_t publish_interval_ms,
                   std::size_t hold_seconds) {
  const auto channels = std::vector<std::string>{
      kImageFront6mm, kImageFront12mm, kPointCloud64};
  std::vector<std::shared_ptr<Node>> writer_nodes;
  auto writers = CreateWriters(channels, &writer_nodes);
  if (writers.size() != channels.size()) {
    return 1;
  }

  if (wait_for_reader) {
    if (!WaitFor(
            [&]() {
              for (const auto& [_, writer] : writers) {
                if (!writer->HasReader()) {
                  return false;
                }
              }
              return true;
            },
            std::chrono::seconds(30))) {
      AERROR << "timed out waiting for topology discovery";
      return 1;
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (!PublishSchedule(schedule, writers,
                       std::chrono::milliseconds(publish_interval_ms))) {
    return 1;
  }
  if (hold_seconds > 0) {
    std::this_thread::sleep_for(std::chrono::seconds(hold_seconds));
  }
  std::cout << "record_play publish only OK: " << schedule.size()
            << " messages" << std::endl;
  return 0;
}

int RunSubscribeOnly(const RecordPlayItems& base_items,
                     const RecordPlaySchedule& schedule) {
  const auto channels = std::vector<std::string>{
      kImageFront6mm, kImageFront12mm, kPointCloud64};
  auto expected_by_channel =
      std::make_shared<std::unordered_map<std::string, RecordPlayItems>>();
  for (const auto& item : schedule) {
    (*expected_by_channel)[item.channel_name].push_back(item);
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<std::string, std::size_t> next_index;
  RecordPlayStats stats;
  std::vector<std::shared_ptr<Node>> reader_nodes;
  std::unordered_map<std::string, ReaderPtr> readers;
  for (const auto& channel : channels) {
    auto node =
        apollo::cyber::CreateNode(MakeNodeName("record_play_sub", channel));
    auto reader = node->CreateReader<transport::PodMessage>(
        channel,
        [&mutex, &cv, &next_index, &stats, expected_by_channel, channel](
            const std::shared_ptr<transport::PodMessage>& msg) {
          std::lock_guard<std::mutex> lock(mutex);
          const auto expected_it = expected_by_channel->find(channel);
          if (expected_it == expected_by_channel->end()) {
            ++stats.mismatches;
            return;
          }
          auto& index = next_index[channel];
          if (index >= expected_it->second.size()) {
            ++stats.duplicates;
            return;
          }
          const auto& expected = expected_it->second[index];
          if (!ValidateChunk(*msg, expected)) {
            ++stats.mismatches;
          } else {
            ++stats.total_messages;
            stats.total_bytes += expected.payload.size() +
                                 sizeof(transport::PodChunkHeader);
          }
          ++index;
          cv.notify_one();
        });
    if (reader == nullptr) {
      return 1;
    }
    reader_nodes.emplace_back(std::move(node));
    readers.emplace(channel, std::move(reader));
  }
  if (readers.size() != channels.size()) {
    return 1;
  }
  if (!WaitFor(
          [&]() {
            for (const auto& [_, reader] : readers) {
              if (!reader->HasWriter()) {
                return false;
              }
            }
            return true;
          },
          std::chrono::seconds(30))) {
    AERROR << "timed out waiting for topology discovery";
    return 1;
  }
  const auto total_expected = schedule.size();
  if (!WaitFor(
          [&]() {
            std::lock_guard<std::mutex> lock(mutex);
            return stats.total_messages == total_expected;
          },
          std::chrono::seconds(120))) {
    AERROR << "timed out waiting for incoming replay messages";
    return 1;
  }
  std::cout << "record_play subscribe only OK: " << ChannelSummary(base_items)
            << ", received=" << stats.total_messages << std::endl;
  return 0;
}

}  // namespace record_play
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  apollo::cyber::Init(argv[0]);
  const auto options = apollo::cyber::examples::record_play::ParseOptions(
      argc, argv);

  apollo::cyber::examples::record_play::RecordPlayItems items;
  if (!apollo::cyber::examples::record_play::LoadRecordPlayItems(
          options.record_path, options.max_per_channel, &items)) {
    return 1;
  }

  const auto schedule =
      apollo::cyber::examples::record_play::ExpandRecordPlaySchedule(
          items, options.repeat);

  int ret = 0;
  if (options.mode == "publish") {
    ret = apollo::cyber::examples::record_play::RunPublishOnly(
        schedule, options.wait_for_reader, options.publish_interval_ms,
        options.hold_seconds);
  } else if (options.mode == "subscribe") {
    ret =
        apollo::cyber::examples::record_play::RunSubscribeOnly(items,
                                                                schedule);
  } else {
    ret = apollo::cyber::examples::record_play::RunRoundTrip(items,
                                                              schedule);
  }

  apollo::cyber::Clear();
  return ret;
}

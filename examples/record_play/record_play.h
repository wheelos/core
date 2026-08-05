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

#ifndef CYBER_EXAMPLES_RECORD_PLAY_RECORD_PLAY_H_
#define CYBER_EXAMPLES_RECORD_PLAY_RECORD_PLAY_H_

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <thread>
#include <vector>

#include <unistd.h>

#include "cyber/common/log.h"
#include "cyber/record/record_reader.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/transport.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace record_play {

constexpr char kDefaultRecordPath[] = "/mnt/synology/apollo/sensor_rgb.record";
constexpr char kImageFront6mm[] = "/apollo/sensor/camera/front_6mm/image";
constexpr char kImageFront12mm[] = "/apollo/sensor/camera/front_12mm/image";
constexpr char kPointCloud64[] =
    "/apollo/sensor/velodyne64/compensator/PointCloud2";

inline bool IsRecordPlayChannel(const std::string& channel_name) {
  return channel_name == kImageFront6mm || channel_name == kImageFront12mm ||
         channel_name == kPointCloud64;
}

inline transport::PodPayloadKind PayloadKindForChannel(
    const std::string& channel_name) {
  if (channel_name == kPointCloud64) {
    return transport::PodPayloadKind::POINT_CLOUD;
  }
  return transport::PodPayloadKind::IMAGE;
}

inline uint64_t HashBytes(const uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  constexpr uint64_t kOffset = 1469598103934665603ULL;
  constexpr uint64_t kPrime = 1099511628211ULL;
  uint64_t hash = kOffset;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= kPrime;
  }
  return hash;
}

struct RecordPlayItem {
  std::string channel_name;
  transport::PodChunkHeader header;
  uint64_t payload_hash = 0;
  std::vector<uint8_t> payload;
};

using RecordPlayItems = std::vector<RecordPlayItem>;
using RecordPlaySchedule = std::vector<RecordPlayItem>;
using ChannelItems = std::map<std::string, RecordPlayItems>;

inline std::string SanitizeName(const std::string& value) {
  std::string result = value;
  std::replace_if(result.begin(), result.end(),
                  [](char c) {
                    return !(std::isalnum(static_cast<unsigned char>(c)) ||
                             c == '_');
                  },
                  '_');
  return result;
}

inline std::string MakeNodeName(const std::string& prefix,
                                const std::string& channel_name) {
  return prefix + "_" + std::to_string(::getpid()) + "_" +
         SanitizeName(channel_name);
}

inline proto::RoleAttributes MakePodRoleAttr(const std::string& channel_name) {
  proto::RoleAttributes attr;
  attr.set_channel_name(channel_name);
  attr.set_channel_id(common::Hash(channel_name));
  attr.set_message_type(transport::PodMessage::TypeName());
  attr.mutable_qos_profile()->CopyFrom(
      transport::QosProfileConf::QOS_PROFILE_DEFAULT);
  return attr;
}

inline bool LoadRecordPlayItems(const std::string& record_path,
                                std::size_t max_per_channel,
                                RecordPlayItems* items) {
  if (items == nullptr || max_per_channel == 0) {
    return false;
  }
  items->clear();
  if (!std::filesystem::exists(record_path)) {
    AERROR << "record file not found: " << record_path;
    return false;
  }

  record::RecordReader reader(record_path);
  if (!reader.IsValid()) {
    AERROR << "invalid record file: " << record_path;
    return false;
  }

  std::map<std::string, std::size_t> per_channel_count;
  record::RecordMessage message;
  while (reader.ReadMessage(&message)) {
    if (!IsRecordPlayChannel(message.channel_name)) {
      continue;
    }
    auto& count = per_channel_count[message.channel_name];
    if (count >= max_per_channel) {
      continue;
    }

    RecordPlayItem item;
    item.channel_name = message.channel_name;
    item.header.payload_kind = static_cast<uint32_t>(
        PayloadKindForChannel(message.channel_name));
    item.header.timestamp_ns = message.time;
    item.header.frame_id = items->size();
    transport::PodChunkView pod_view;
    bool preserve_header = false;
    if (transport::ParsePodChunk(message.content.data(), message.content.size(),
                                 &pod_view) &&
        pod_view.payload != nullptr) {
      item.header = pod_view.header;
      item.payload.assign(pod_view.payload,
                          pod_view.payload + pod_view.payload_size);
      preserve_header = true;
    } else {
      item.payload.assign(message.content.begin(), message.content.end());
    }
    item.payload_hash = HashBytes(item.payload.data(), item.payload.size());
    item.header.payload_size = static_cast<uint32_t>(item.payload.size());
    if (!preserve_header) {
      item.header.schema_hash = static_cast<uint32_t>(item.payload_hash);
    }
    items->push_back(std::move(item));
    ++count;
  }

  if (items->empty()) {
    AERROR << "no image/pointcloud messages found in record: " << record_path;
    return false;
  }
  if (per_channel_count[kImageFront6mm] == 0 ||
      per_channel_count[kImageFront12mm] == 0 ||
      per_channel_count[kPointCloud64] == 0) {
    AERROR << "record missing required image/pointcloud channels: "
           << record_path;
    return false;
  }
  return true;
}

inline ChannelItems GroupByChannel(const RecordPlayItems& items) {
  ChannelItems grouped;
  for (const auto& item : items) {
    grouped[item.channel_name].push_back(item);
  }
  return grouped;
}

inline RecordPlaySchedule ExpandRecordPlaySchedule(const RecordPlayItems& items,
                                                   std::size_t repeat) {
  RecordPlaySchedule schedule;
  if (repeat == 0) {
    repeat = 1;
  }
  schedule.reserve(items.size() * repeat);
  for (std::size_t round = 0; round < repeat; ++round) {
    for (const auto& item : items) {
      schedule.push_back(item);
    }
  }
  return schedule;
}

inline transport::PodChunkHeader MakeHeader(const RecordPlayItem& item) {
  return item.header;
}

inline bool HeadersEqual(const transport::PodChunkHeader& lhs,
                         const transport::PodChunkHeader& rhs) {
  return lhs.magic == rhs.magic && lhs.version == rhs.version &&
         lhs.header_size == rhs.header_size &&
         lhs.payload_kind == rhs.payload_kind &&
         lhs.timestamp_ns == rhs.timestamp_ns &&
         lhs.frame_id == rhs.frame_id && lhs.width == rhs.width &&
         lhs.height == rhs.height && lhs.stride_bytes == rhs.stride_bytes &&
         lhs.pixel_format == rhs.pixel_format &&
         lhs.payload_size == rhs.payload_size &&
         lhs.schema_hash == rhs.schema_hash &&
         std::equal(std::begin(lhs.reserved), std::end(lhs.reserved),
                    std::begin(rhs.reserved));
}

inline bool BuildChunk(const RecordPlayItem& item, std::vector<uint8_t>* chunk) {
  if (chunk == nullptr) {
    return false;
  }
  chunk->resize(transport::PodChunkTotalSize(item.payload.size()));
  std::size_t written = 0;
  const auto header = MakeHeader(item);
  return transport::BuildPodChunk(header, item.payload.data(),
                                  item.payload.size(), chunk->data(),
                                  chunk->size(), &written) &&
         written == chunk->size();
}

template <typename WriterMap>
inline bool PublishSchedule(const RecordPlaySchedule& schedule,
                            const WriterMap& writers,
                            std::chrono::milliseconds interval =
                                std::chrono::milliseconds(0)) {
  for (const auto& item : schedule) {
    const auto writer_it = writers.find(item.channel_name);
    if (writer_it == writers.end()) {
      AERROR << "missing writer for " << item.channel_name;
      return false;
    }
    auto& writer = writer_it->second;
    auto msg = std::make_shared<transport::PodMessage>(MakeHeader(item),
                                                       item.payload.data(),
                                                       item.payload.size());
    if (!writer->Write(msg)) {
      AERROR << "publish failed for " << item.channel_name;
      return false;
    }
    if (interval.count() > 0) {
      std::this_thread::sleep_for(interval);
    }
  }
  return true;
}

inline bool ValidateChunk(const transport::PodMessage& message,
                          const RecordPlayItem& item) {
  const auto* header = message.header();
  if (header == nullptr) {
    return false;
  }
  if (!HeadersEqual(*header, item.header)) {
    return false;
  }
  const auto view = message.View();
  return view.payload != nullptr &&
         view.payload_size == item.payload.size() &&
         std::memcmp(view.payload, item.payload.data(), item.payload.size()) ==
             0;
}

inline std::string ChannelSummary(const RecordPlayItems& items) {
  std::map<std::string, std::size_t> counts;
  std::size_t total_bytes = 0;
  for (const auto& item : items) {
    ++counts[item.channel_name];
    total_bytes += item.payload.size();
  }
  std::string summary;
  for (const auto& [channel, count] : counts) {
    summary += channel + "=" + std::to_string(count) + " ";
  }
  summary += "bytes=" + std::to_string(total_bytes);
  return summary;
}

struct RecordPlayStats {
  std::size_t total_messages = 0;
  std::size_t total_bytes = 0;
  std::size_t mismatches = 0;
  std::size_t duplicates = 0;
  double throughput_mb_s = 0.0;
  double throughput_msg_s = 0.0;
};

template <typename Predicate>
inline bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

}  // namespace record_play
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_EXAMPLES_RECORD_PLAY_RECORD_PLAY_H_

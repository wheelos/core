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

#ifndef CYBER_EXAMPLES_RECORD_PLAY_RECORD_PLAY_TOOL_H_
#define CYBER_EXAMPLES_RECORD_PLAY_RECORD_PLAY_TOOL_H_

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cyber/common/log.h"
#include "cyber/message/message_traits.h"
#include "cyber/message/raw_message.h"
#include "cyber/message/protobuf_factory.h"
#include "cyber/record/record_reader.h"
#include "cyber/record/record_writer.h"
#include "cyber/transport/message/pod_message.h"
#include "examples/record_play/record_play.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace record_play {

struct RecordPlayBenchmarkResult {
  std::size_t messages = 0;
  std::size_t bytes = 0;
  double elapsed_seconds = 0.0;
  double throughput_mb_s = 0.0;
  double throughput_msg_s = 0.0;
};

struct ConvertedRecordResult {
  std::size_t messages = 0;
  std::size_t bytes = 0;
  std::size_t channels = 0;
};

inline bool WriteTextFile(const std::string& path, const std::string& content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  if (!ofs.is_open()) {
    AERROR << "failed to open file: " << path;
    return false;
  }
  ofs << content;
  return ofs.good();
}

inline std::string BuildManifestLine(const RecordPlayItem& item) {
  return item.channel_name + "\t" + std::to_string(item.header.timestamp_ns) + "\t" +
         std::to_string(item.payload.size()) + "\t" +
         std::to_string(item.payload_hash) + "\n";
}

inline bool ConvertRecordToPod(const std::string& source_record,
                               const std::string& output_record,
                               const std::string& manifest_path,
                               std::size_t max_per_channel,
                               ConvertedRecordResult* result) {
  if (result == nullptr) {
    return false;
  }
  result->messages = 0;
  result->bytes = 0;
  result->channels = 0;

  RecordPlayItems items;
  if (!LoadRecordPlayItems(source_record, max_per_channel, &items)) {
    return false;
  }
  const auto grouped = GroupByChannel(items);
  result->channels = grouped.size();

  record::RecordWriter writer;
  writer.SetSizeOfFileSegmentation(0);
  writer.SetIntervalOfFileSegmentation(0);
  if (!writer.Open(output_record)) {
    return false;
  }

  for (const auto& entry : grouped) {
    if (!writer.WriteChannel(entry.first,
                             transport::PodMessage::TypeName(),
                             transport::PodSchemaDescriptor())) {
      writer.Close();
      return false;
    }
  }

  std::string manifest;
  manifest.reserve(items.size() * 64);
  for (const auto& item : items) {
    transport::PodMessage pod(MakeHeader(item), item.payload.data(),
                              item.payload.size());
    std::string encoded;
    if (!pod.SerializeToString(&encoded)) {
      writer.Close();
      return false;
    }
    if (!writer.WriteMessage(item.channel_name, encoded,
                             item.header.timestamp_ns)) {
      writer.Close();
      return false;
    }
    manifest += BuildManifestLine(item);
    ++result->messages;
    result->bytes += encoded.size();
  }
  writer.Close();

  if (!manifest_path.empty() && !WriteTextFile(manifest_path, manifest)) {
    return false;
  }
  return true;
}

inline RecordPlayBenchmarkResult BenchmarkProtobufDecode(
    const std::string& record, std::size_t max_per_channel = 64) {
  RecordPlayBenchmarkResult stats;
  if (max_per_channel == 0) {
    return stats;
  }
  record::RecordReader reader(record);
  if (!reader.IsValid()) {
    return stats;
  }

  auto* factory = message::ProtobufFactory::Instance();
  for (const auto& channel : reader.GetChannelList()) {
    const auto& proto_desc = reader.GetProtoDesc(channel);
    const auto& message_type = reader.GetMessageType(channel);
    if (!proto_desc.empty() &&
        message_type != transport::PodMessage::TypeName() &&
        !transport::IsPodSchemaDescriptor(proto_desc)) {
      factory->RegisterMessage(proto_desc);
    }
  }

  const auto start = std::chrono::steady_clock::now();
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
    const auto& type = reader.GetMessageType(message.channel_name);
    std::unique_ptr<google::protobuf::Message> dynamic_msg(
        factory->GenerateMessageByType(type));
    if (dynamic_msg == nullptr) {
      continue;
    }
    if (!message::ParseFromArray(message.content.data(),
                                 static_cast<int>(message.content.size()),
                                 dynamic_msg.get())) {
      continue;
    }
    stats.bytes += message.content.size();
    ++stats.messages;
    ++count;
  }
  const auto end = std::chrono::steady_clock::now();
  stats.elapsed_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start)
          .count();
  stats.throughput_mb_s =
      (static_cast<double>(stats.bytes) / (1024.0 * 1024.0)) /
      std::max(0.001, stats.elapsed_seconds);
  stats.throughput_msg_s = static_cast<double>(stats.messages) /
                           std::max(0.001, stats.elapsed_seconds);
  return stats;
}

inline RecordPlayBenchmarkResult BenchmarkPodBorrow(const std::string& record) {
  RecordPlayBenchmarkResult stats;
  record::RecordReader reader(record);
  if (!reader.IsValid()) {
    return stats;
  }

  const auto start = std::chrono::steady_clock::now();
  record::RecordMessage message;
  while (reader.ReadMessage(&message)) {
    if (!IsRecordPlayChannel(message.channel_name)) {
      continue;
    }
    transport::PodMessage pod;
    if (!pod.BorrowFromArray(message.content.data(), message.content.size())) {
      continue;
    }
    if (pod.header() == nullptr) {
      continue;
    }
    stats.bytes += message.content.size();
    ++stats.messages;
  }
  const auto end = std::chrono::steady_clock::now();
  stats.elapsed_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start)
          .count();
  stats.throughput_mb_s =
      (static_cast<double>(stats.bytes) / (1024.0 * 1024.0)) /
      std::max(0.001, stats.elapsed_seconds);
  stats.throughput_msg_s = static_cast<double>(stats.messages) /
                           std::max(0.001, stats.elapsed_seconds);
  return stats;
}

inline bool DumpConvertedRecord(const std::string& record,
                                const std::string& dump_dir,
                                std::size_t max_per_channel) {
  if (dump_dir.empty()) {
    return false;
  }
  std::filesystem::create_directories(dump_dir);
  RecordPlayItems items;
  if (!LoadRecordPlayItems(record, max_per_channel, &items)) {
    return false;
  }

  std::map<std::string, std::size_t> per_channel_index;
  for (const auto& item : items) {
    transport::PodMessage pod(MakeHeader(item), item.payload.data(),
                              item.payload.size());
    const auto view = pod.View();
    if (view.payload == nullptr) {
      return false;
    }

    const auto channel_dir =
        dump_dir + "/" + SanitizeName(item.channel_name);
    std::filesystem::create_directories(channel_dir);
    const auto index = per_channel_index[item.channel_name]++;
    const auto base = channel_dir + "/" + std::to_string(index) + "_" +
                      std::to_string(item.header.timestamp_ns);
    const auto bin_path = base + ".bin";
    std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
    if (!bin.is_open()) {
      AERROR << "failed to open dump file: " << bin_path;
      return false;
    }
    bin.write(reinterpret_cast<const char*>(view.payload), view.payload_size);
    if (!bin.good()) {
      return false;
    }
    const std::string meta = "channel=" + item.channel_name +
                             "\ntimestamp_ns=" +
                             std::to_string(item.header.timestamp_ns) +
                             "\npayload_size=" +
                             std::to_string(view.payload_size) +
                             "\npayload_hash=" +
                             std::to_string(item.payload_hash) + "\n";
    if (!WriteTextFile(base + ".meta", meta)) {
      return false;
    }
  }
  return true;
}

}  // namespace record_play
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_EXAMPLES_RECORD_PLAY_RECORD_PLAY_TOOL_H_

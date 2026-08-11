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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

struct MixedRecordTypeStats {
  std::size_t messages = 0;
  std::size_t bytes = 0;
};

struct RecordPlayBenchmarkResult {
  std::size_t messages = 0;
  std::size_t bytes = 0;
  std::size_t failed_messages = 0;
  double elapsed_seconds = 0.0;
  double read_seconds = 0.0;
  double process_seconds = 0.0;
  double throughput_mb_s = 0.0;
  double throughput_msg_s = 0.0;
  std::map<std::string, MixedRecordTypeStats> by_type;
};

struct ConvertedRecordResult {
  std::size_t messages = 0;
  std::size_t bytes = 0;
  std::size_t channels = 0;
};

struct MixedConvertedRecordResult {
  std::size_t messages = 0;
  std::size_t bytes = 0;
  std::size_t channels = 0;
  std::size_t pod_messages = 0;
  std::size_t protobuf_messages = 0;
  std::map<std::string, MixedRecordTypeStats> by_type;
};

inline const google::protobuf::FieldDescriptor* FindField(
    const google::protobuf::Descriptor* descriptor,
    std::initializer_list<const char*> names) {
  if (descriptor == nullptr) {
    return nullptr;
  }
  for (const auto* name : names) {
    if (const auto* field = descriptor->FindFieldByName(name)) {
      return field;
    }
  }
  return nullptr;
}

inline bool ReadUint32Field(
    const google::protobuf::Message& message,
    const google::protobuf::FieldDescriptor* field, uint32_t* value) {
  if (field == nullptr || value == nullptr || field->is_repeated()) {
    return false;
  }
  const auto* reflection = message.GetReflection();
  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      *value = reflection->GetUInt32(message, field);
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
      *value = static_cast<uint32_t>(reflection->GetUInt64(message, field));
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      *value = static_cast<uint32_t>(reflection->GetInt32(message, field));
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      *value = static_cast<uint32_t>(reflection->GetInt64(message, field));
      return true;
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      *value = static_cast<uint32_t>(
          reflection->GetEnumValue(message, field));
      return true;
    default:
      return false;
  }
}

inline void AppendPodBytes(const void* value, std::size_t size,
                           std::vector<uint8_t>* payload) {
  const auto* bytes = static_cast<const uint8_t*>(value);
  payload->insert(payload->end(), bytes, bytes + size);
}

inline bool ConvertPointCloudMessage(
    const google::protobuf::Message& protobuf, uint64_t timestamp_ns,
    std::vector<uint8_t>* payload, transport::PodChunkHeader* header) {
  if (payload == nullptr || header == nullptr) {
    return false;
  }
  payload->clear();
  const auto* descriptor = protobuf.GetDescriptor();
  const auto* reflection = protobuf.GetReflection();
  const auto* point_field = FindField(descriptor, {"point", "points"});
  if (point_field != nullptr && point_field->is_repeated() &&
      point_field->cpp_type() ==
          google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    const auto* point_descriptor = point_field->message_type();
    const auto* x_field = FindField(point_descriptor, {"x"});
    const auto* y_field = FindField(point_descriptor, {"y"});
    const auto* z_field = FindField(point_descriptor, {"z"});
    const auto* intensity_field = FindField(point_descriptor, {"intensity"});
    const auto* point_timestamp_field =
        FindField(point_descriptor, {"timestamp", "time"});
    if (x_field == nullptr || y_field == nullptr || z_field == nullptr ||
        intensity_field == nullptr) {
      return false;
    }
    for (int i = 0; i < reflection->FieldSize(protobuf, point_field); ++i) {
      const auto& point =
          reflection->GetRepeatedMessage(protobuf, point_field, i);
      const auto* point_reflection = point.GetReflection();
      const float x = point_reflection->GetFloat(point, x_field);
      const float y = point_reflection->GetFloat(point, y_field);
      const float z = point_reflection->GetFloat(point, z_field);
      uint32_t intensity = 0;
      if (intensity_field->cpp_type() ==
          google::protobuf::FieldDescriptor::CPPTYPE_UINT32) {
        intensity = point_reflection->GetUInt32(point, intensity_field);
      } else if (intensity_field->cpp_type() ==
                 google::protobuf::FieldDescriptor::CPPTYPE_INT32) {
        intensity = static_cast<uint32_t>(
            point_reflection->GetInt32(point, intensity_field));
      } else if (intensity_field->cpp_type() ==
                 google::protobuf::FieldDescriptor::CPPTYPE_UINT64) {
        intensity = static_cast<uint32_t>(
            point_reflection->GetUInt64(point, intensity_field));
      } else {
        return false;
      }
      uint64_t point_timestamp = timestamp_ns;
      if (point_timestamp_field != nullptr &&
          point_timestamp_field->cpp_type() ==
              google::protobuf::FieldDescriptor::CPPTYPE_UINT64) {
        point_timestamp =
            point_reflection->GetUInt64(point, point_timestamp_field);
      }
      AppendPodBytes(&x, sizeof(x), payload);
      AppendPodBytes(&y, sizeof(y), payload);
      AppendPodBytes(&z, sizeof(z), payload);
      AppendPodBytes(&intensity, sizeof(intensity), payload);
      AppendPodBytes(&point_timestamp, sizeof(point_timestamp), payload);
    }
    header->width = 0;
    header->height = 0;
    (void)ReadUint32Field(protobuf, FindField(descriptor, {"width"}),
                          &header->width);
    (void)ReadUint32Field(protobuf, FindField(descriptor, {"height"}),
                          &header->height);
    header->stride_bytes = 24;
    return true;
  }

  const auto* data_field = FindField(descriptor, {"data"});
  const auto* fields_field = FindField(descriptor, {"fields"});
  uint32_t point_step = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  if (data_field == nullptr || fields_field == nullptr ||
      !fields_field->is_repeated() ||
      data_field->cpp_type() !=
          google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
      !ReadUint32Field(protobuf, FindField(descriptor, {"point_step"}),
                       &point_step) ||
      !ReadUint32Field(protobuf, FindField(descriptor, {"width"}), &width) ||
      !ReadUint32Field(protobuf, FindField(descriptor, {"height"}), &height)) {
    return false;
  }
  const std::string& source = reflection->GetString(protobuf, data_field);
  struct FieldLocation {
    uint32_t offset = 0;
    uint32_t datatype = 0;
    bool found = false;
  };
  FieldLocation x, y, z, intensity, timestamp_location;
  const auto* field_descriptor = fields_field->message_type();
  const auto* name_field = FindField(field_descriptor, {"name"});
  const auto* offset_field = FindField(field_descriptor, {"offset"});
  const auto* datatype_field = FindField(field_descriptor, {"datatype"});
  if (name_field == nullptr || offset_field == nullptr ||
      datatype_field == nullptr) {
    return false;
  }
  for (int i = 0; i < reflection->FieldSize(protobuf, fields_field); ++i) {
    const auto& field =
        reflection->GetRepeatedMessage(protobuf, fields_field, i);
    const auto* field_reflection = field.GetReflection();
    const std::string& name = field_reflection->GetString(field, name_field);
    FieldLocation* location = nullptr;
    if (name == "x") {
      location = &x;
    } else if (name == "y") {
      location = &y;
    } else if (name == "z") {
      location = &z;
    } else if (name == "intensity") {
      location = &intensity;
    } else if (name == "timestamp" || name == "time") {
      location = &timestamp_location;
    }
    if (location != nullptr) {
      if (!ReadUint32Field(field, offset_field, &location->offset) ||
          !ReadUint32Field(field, datatype_field, &location->datatype)) {
        return false;
      }
      location->found = true;
    }
  }
  if (!x.found || !y.found || !z.found || !intensity.found ||
      point_step == 0) {
    return false;
  }
  const std::size_t inferred_count = source.size() / point_step;
  const std::size_t point_count =
      width != 0 && height != 0 ? static_cast<std::size_t>(width) * height
                               : inferred_count;
  if (point_count * point_step > source.size()) {
    return false;
  }
  bool big_endian = false;
  const auto* endian_field = FindField(descriptor, {"is_bigendian"});
  if (endian_field != nullptr &&
      endian_field->cpp_type() ==
          google::protobuf::FieldDescriptor::CPPTYPE_BOOL) {
    big_endian = reflection->GetBool(protobuf, endian_field);
  }
  const auto read_numeric = [&](const FieldLocation& location,
                                const uint8_t* point, double* value) {
    if (location.offset >= point_step || value == nullptr) {
      return false;
    }
    std::size_t width_bytes = 0;
    switch (location.datatype) {
      case 2:
        width_bytes = 1;
        break;
      case 3:
      case 4:
        width_bytes = 2;
        break;
      case 1:
      case 5:
      case 6:
      case 7:
        width_bytes = 4;
        break;
      case 8:
        width_bytes = 8;
        break;
      default:
        return false;
    }
    if (location.offset + width_bytes > point_step) {
      return false;
    }
    uint8_t normalized[sizeof(double)] = {};
    const auto* bytes = point + location.offset;
    if (big_endian) {
      for (std::size_t i = 0; i < width_bytes; ++i) {
        normalized[i] = bytes[width_bytes - i - 1];
      }
      bytes = normalized;
    }
    switch (location.datatype) {
      case 1: {
        *value = static_cast<int8_t>(bytes[0]);
        return true;
      }
      case 2:
        *value = bytes[0];
        return true;
      case 3: {
        int16_t v = 0;
        std::memcpy(&v, bytes, sizeof(v));
        *value = v;
        return true;
      }
      case 4: {
        uint16_t v = 0;
        std::memcpy(&v, bytes, sizeof(v));
        *value = v;
        return true;
      }
      case 5: {
        int32_t v = 0;
        std::memcpy(&v, bytes, sizeof(v));
        *value = v;
        return true;
      }
      case 6: {
        uint32_t v = 0;
        std::memcpy(&v, bytes, sizeof(v));
        *value = v;
        return true;
      }
      case 7: {
        float v = 0;
        std::memcpy(&v, bytes, sizeof(v));
        *value = v;
        return true;
      }
      case 8: {
        double v = 0;
        std::memcpy(&v, bytes, sizeof(v));
        *value = v;
        return true;
      }
      default:
        return false;
    }
  };
  for (std::size_t i = 0; i < point_count; ++i) {
    const auto* point =
        reinterpret_cast<const uint8_t*>(source.data()) + i * point_step;
    double value = 0;
    float xyz[3] = {};
    if (!read_numeric(x, point, &value)) {
      return false;
    }
    xyz[0] = static_cast<float>(value);
    if (!read_numeric(y, point, &value)) {
      return false;
    }
    xyz[1] = static_cast<float>(value);
    if (!read_numeric(z, point, &value)) {
      return false;
    }
    xyz[2] = static_cast<float>(value);
    if (!read_numeric(intensity, point, &value)) {
      return false;
    }
    const uint32_t intensity_value = static_cast<uint32_t>(value);
    uint64_t point_timestamp = timestamp_ns;
    if (timestamp_location.found &&
        read_numeric(timestamp_location, point, &value)) {
      point_timestamp = static_cast<uint64_t>(value);
    }
    AppendPodBytes(xyz, sizeof(xyz), payload);
    AppendPodBytes(&intensity_value, sizeof(intensity_value), payload);
    AppendPodBytes(&point_timestamp, sizeof(point_timestamp), payload);
  }
  header->width = width;
  header->height = height;
  header->stride_bytes = 24;
  return true;
}

inline bool ConvertSensorMessageToPod(
    const std::string& channel_name, const std::string& message_type,
    const std::string& content, uint64_t timestamp_ns, uint64_t frame_id,
    std::string* encoded) {
  if (encoded == nullptr) {
    return false;
  }
  transport::PodChunkView existing_view;
  if (transport::ParsePodChunk(content.data(), content.size(), &existing_view)) {
    *encoded = content;
    return true;
  }
  auto* factory = message::ProtobufFactory::Instance();
  std::unique_ptr<google::protobuf::Message> protobuf(
      factory->GenerateMessageByType(message_type));
  if (protobuf == nullptr ||
      !message::ParseFromArray(content.data(), static_cast<int>(content.size()),
                               protobuf.get())) {
    AERROR << "failed to decode sensor protobuf for " << channel_name
           << " type=" << message_type;
    return false;
  }
  transport::PodChunkHeader header;
  header.payload_kind = static_cast<uint32_t>(PayloadKindForChannel(channel_name));
  header.timestamp_ns = timestamp_ns;
  header.frame_id = frame_id;
  std::vector<uint8_t> payload;
  if (channel_name == kPointCloud64) {
    if (!ConvertPointCloudMessage(*protobuf, timestamp_ns, &payload, &header)) {
      AERROR << "failed to convert PointCloud2 payload for " << channel_name;
      return false;
    }
  } else {
    const auto* descriptor = protobuf->GetDescriptor();
    const auto* reflection = protobuf->GetReflection();
    const auto* data_field = FindField(descriptor, {"data"});
    if (data_field == nullptr || data_field->is_repeated() ||
        data_field->cpp_type() !=
            google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
      AERROR << "sensor image has no byte data field for " << channel_name;
      return false;
    }
    const std::string& data = reflection->GetString(*protobuf, data_field);
    payload.assign(data.begin(), data.end());
    (void)ReadUint32Field(*protobuf, FindField(descriptor, {"width"}),
                          &header.width);
    (void)ReadUint32Field(*protobuf, FindField(descriptor, {"height"}),
                          &header.height);
    (void)ReadUint32Field(*protobuf,
                          FindField(descriptor, {"step", "row_step"}),
                          &header.stride_bytes);
    const auto* encoding_field = FindField(descriptor, {"encoding"});
    if (encoding_field != nullptr &&
        encoding_field->cpp_type() ==
            google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
        !encoding_field->is_repeated()) {
      const std::string& encoding =
          reflection->GetString(*protobuf, encoding_field);
      header.pixel_format = static_cast<uint32_t>(
          HashBytes(reinterpret_cast<const uint8_t*>(encoding.data()),
                    encoding.size()));
    }
  }
  header.payload_size = static_cast<uint32_t>(payload.size());
  header.schema_hash = static_cast<uint32_t>(HashBytes(
      payload.data(), payload.size()));
  transport::PodMessage pod(header, payload.data(), payload.size());
  return pod.SerializeToString(encoded);
}

inline bool WriteTextFile(const std::string& path, const std::string& content);

inline bool ConvertRecordToMixedPod(
    const std::string& source_record, const std::string& output_record,
    const std::string& manifest_path, std::size_t max_per_channel,
    MixedConvertedRecordResult* result) {
  if (result == nullptr) {
    return false;
  }
  *result = MixedConvertedRecordResult();
  record::RecordReader reader(source_record);
  if (!reader.IsValid()) {
    AERROR << "invalid source record: " << source_record;
    return false;
  }
  const auto channels = reader.GetChannelList();
  result->channels = channels.size();
  for (const auto& channel : channels) {
    const auto& descriptor = reader.GetProtoDesc(channel);
    if (IsRecordPlayChannel(channel) && !descriptor.empty() &&
        !transport::IsPodSchemaDescriptor(descriptor) &&
        !message::ProtobufFactory::Instance()->RegisterMessage(descriptor)) {
      AERROR << "failed to register protobuf descriptor for " << channel;
      return false;
    }
  }

  std::map<std::string, std::size_t> per_channel_count;
  record::RecordWriter writer;
  writer.SetSizeOfFileSegmentation(0);
  writer.SetIntervalOfFileSegmentation(0);
  if (!writer.Open(output_record)) {
    AERROR << "failed to open mixed output record: " << output_record;
    return false;
  }
  for (const auto& channel : channels) {
    const auto type = IsRecordPlayChannel(channel)
                          ? transport::PodMessage::TypeName()
                          : reader.GetMessageType(channel);
    const auto descriptor = IsRecordPlayChannel(channel)
                                ? transport::PodSchemaDescriptor()
                                : reader.GetProtoDesc(channel);
    if (!writer.WriteChannel(channel, type, descriptor)) {
      writer.Close();
      return false;
    }
  }
  std::ofstream manifest;
  if (!manifest_path.empty()) {
    manifest.open(manifest_path, std::ios::out | std::ios::trunc);
    if (!manifest.is_open()) {
      writer.Close();
      AERROR << "failed to open manifest: " << manifest_path;
      return false;
    }
  }

  record::RecordMessage message;
  while (reader.ReadMessage(&message)) {
    auto& channel_count = per_channel_count[message.channel_name];
    if (max_per_channel != 0 && channel_count >= max_per_channel) {
      continue;
    }

    const bool is_pod = IsRecordPlayChannel(message.channel_name);
    std::string content;
    if (is_pod) {
      if (!ConvertSensorMessageToPod(
              message.channel_name, reader.GetMessageType(message.channel_name),
              message.content, message.time, result->messages, &content)) {
        writer.Close();
        return false;
      }
    } else {
      content = message.content;
    }

    if (!writer.WriteMessage(message.channel_name, content, message.time)) {
      AERROR << "failed to write mixed message on "
             << message.channel_name;
      writer.Close();
      return false;
    }
    ++channel_count;
    ++result->messages;
    result->bytes += content.size();
    const auto type = is_pod ? transport::PodMessage::TypeName()
                             : reader.GetMessageType(message.channel_name);
    if (is_pod) {
      ++result->pod_messages;
    } else {
      ++result->protobuf_messages;
    }
    auto& type_stats = result->by_type[type];
    ++type_stats.messages;
    type_stats.bytes += content.size();
    if (manifest.is_open()) {
      manifest << message.channel_name << "\t" << message.time << "\t"
               << type << "\t" << content.size() << "\n";
      if (!manifest.good()) {
        writer.Close();
        return false;
      }
    }
  }
  writer.Close();
  return true;
}

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

inline RecordPlayBenchmarkResult BenchmarkFullProtobufRecord(
    const std::string& record, std::size_t max_per_channel = 0) {
  RecordPlayBenchmarkResult stats;
  record::RecordReader reader(record);
  if (!reader.IsValid()) {
    return stats;
  }
  auto* factory = message::ProtobufFactory::Instance();
  for (const auto& channel : reader.GetChannelList()) {
    const auto& descriptor = reader.GetProtoDesc(channel);
    if (!descriptor.empty() && !transport::IsPodSchemaDescriptor(descriptor) &&
        !factory->RegisterMessage(descriptor)) {
      ++stats.failed_messages;
      return stats;
    }
  }
  std::map<std::string, std::size_t> per_channel_count;
  const auto start = std::chrono::steady_clock::now();
  record::RecordMessage message;
  while (true) {
    const auto read_start = std::chrono::steady_clock::now();
    if (!reader.ReadMessage(&message)) {
      break;
    }
    const auto process_start = std::chrono::steady_clock::now();
    stats.read_seconds +=
        std::chrono::duration_cast<std::chrono::duration<double>>(
            process_start - read_start)
            .count();
    auto& count = per_channel_count[message.channel_name];
    if (max_per_channel != 0 && count >= max_per_channel) {
      continue;
    }
    const auto type = reader.GetMessageType(message.channel_name);
    std::unique_ptr<google::protobuf::Message> protobuf(
        factory->GenerateMessageByType(type));
    if (protobuf == nullptr ||
        !message::ParseFromArray(message.content.data(),
                                 static_cast<int>(message.content.size()),
                                 protobuf.get())) {
      ++stats.failed_messages;
      continue;
    }
    ++count;
    ++stats.messages;
    stats.bytes += message.content.size();
    auto& type_stats = stats.by_type[type];
    ++type_stats.messages;
    type_stats.bytes += message.content.size();
    stats.process_seconds +=
        std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::steady_clock::now() - process_start)
            .count();
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

inline RecordPlayBenchmarkResult BenchmarkMixedRecord(
    const std::string& record, std::size_t max_per_channel = 0) {
  RecordPlayBenchmarkResult stats;
  record::RecordReader reader(record);
  if (!reader.IsValid()) {
    return stats;
  }
  auto* factory = message::ProtobufFactory::Instance();
  for (const auto& channel : reader.GetChannelList()) {
    const auto& descriptor = reader.GetProtoDesc(channel);
    if (!IsRecordPlayChannel(channel) && !descriptor.empty() &&
        !transport::IsPodSchemaDescriptor(descriptor) &&
        !factory->RegisterMessage(descriptor)) {
      ++stats.failed_messages;
      return stats;
    }
  }
  std::map<std::string, std::size_t> per_channel_count;
  const auto start = std::chrono::steady_clock::now();
  record::RecordMessage message;
  while (true) {
    const auto read_start = std::chrono::steady_clock::now();
    if (!reader.ReadMessage(&message)) {
      break;
    }
    const auto process_start = std::chrono::steady_clock::now();
    stats.read_seconds +=
        std::chrono::duration_cast<std::chrono::duration<double>>(
            process_start - read_start)
            .count();
    auto& count = per_channel_count[message.channel_name];
    if (max_per_channel != 0 && count >= max_per_channel) {
      continue;
    }
    const auto type = reader.GetMessageType(message.channel_name);
    bool valid = false;
    if (IsRecordPlayChannel(message.channel_name) ||
        type == transport::PodMessage::TypeName()) {
      transport::PodMessage pod;
      valid = pod.BorrowFromArray(message.content.data(),
                                  message.content.size());
    } else {
      std::unique_ptr<google::protobuf::Message> protobuf(
          factory->GenerateMessageByType(type));
      valid = protobuf != nullptr &&
              message::ParseFromArray(
                  message.content.data(),
                  static_cast<int>(message.content.size()), protobuf.get());
    }
    if (!valid) {
      ++stats.failed_messages;
      continue;
    }
    ++count;
    ++stats.messages;
    stats.bytes += message.content.size();
    auto& type_stats = stats.by_type[type];
    ++type_stats.messages;
    type_stats.bytes += message.content.size();
    stats.process_seconds +=
        std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::steady_clock::now() - process_start)
            .count();
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

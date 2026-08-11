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

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"

#include "google/protobuf/any.pb.h"
#include "cyber/cyber.h"
#include "examples/record_play/record_play_tool.h"
#include "cyber/message/raw_message.h"
#include "cyber/transport/message/pod_message.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace record_play {
namespace {

constexpr char kTestSource[] = "record_play_tool_test_source.record";
constexpr char kTestPodSource[] = "record_play_tool_test_pod_source.record";
constexpr char kTestConverted[] = "record_play_tool_test_converted.record";
constexpr char kTestManifest[] = "record_play_tool_test.manifest";
constexpr char kTestMixedSource[] = "record_play_tool_test_mixed_source.record";

struct FakeWriter {
  explicit FakeWriter(bool write_result) : write_result(write_result) {}

  bool Write(const std::shared_ptr<transport::PodMessage>&) {
    ++calls;
    return write_result;
  }

  bool write_result = true;
  int calls = 0;
};

void CleanupArtifacts() {
  (void)std::remove(kTestSource);
  (void)std::remove(kTestPodSource);
  (void)std::remove(kTestConverted);
  (void)std::remove(kTestManifest);
  (void)std::remove(kTestMixedSource);
}

void WriteSourceRecord() {
  record::RecordWriter writer;
  writer.SetSizeOfFileSegmentation(0);
  writer.SetIntervalOfFileSegmentation(0);
  ASSERT_TRUE(writer.Open(kTestSource));

  const std::vector<std::string> channels = {
      kImageFront6mm, kImageFront12mm, kPointCloud64};
  for (const auto& channel : channels) {
    for (int i = 0; i < 3; ++i) {
      auto msg = std::make_shared<message::RawMessage>(
          channel + "_" + std::to_string(i));
      ASSERT_TRUE(writer.WriteMessage(channel, msg, 1000 + i));
    }
  }
  writer.Close();
}

bool WritePodSourceRecord(RecordPlayItems* expected) {
  if (expected == nullptr) {
    return false;
  }
  expected->clear();

  record::RecordWriter writer;
  writer.SetSizeOfFileSegmentation(0);
  writer.SetIntervalOfFileSegmentation(0);
  if (!writer.Open(kTestPodSource)) {
    return false;
  }

  struct ChannelSpec {
    const char* channel;
    transport::PodChunkHeader header;
    std::string payload;
  };

  const std::vector<ChannelSpec> specs = {
      {kImageFront6mm,
       transport::MakeImagePodChunkHeader(1001, 11, 1920, 1080, 3840, 7, 6,
                                          0x10203040u),
       "front6"},
      {kImageFront12mm,
       transport::MakeImagePodChunkHeader(1002, 12, 1280, 720, 2560, 9, 7,
                                          0x55667788u),
       "front12"},
      {kPointCloud64,
       transport::PodChunkHeader{
           transport::PodChunkHeader::kMagic,
           transport::PodChunkHeader::kVersion,
           sizeof(transport::PodChunkHeader),
           static_cast<uint32_t>(transport::PodPayloadKind::POINT_CLOUD),
           1003,
           13,
           64,
           32,
           2048,
           3,
           5,
           0x99AABBCCu,
           {1, 2, 3, 4}},
       "cloud"},
  };

  for (const auto& spec : specs) {
    if (!writer.WriteChannel(spec.channel, transport::PodMessage::TypeName(),
                             transport::PodSchemaDescriptor())) {
      writer.Close();
      return false;
    }
    transport::PodMessage pod(spec.header, spec.payload.data(),
                              spec.payload.size());
    std::string encoded;
    if (!pod.SerializeToString(&encoded) ||
        !writer.WriteMessage(spec.channel, encoded, spec.header.timestamp_ns)) {
      writer.Close();
      return false;
    }

    RecordPlayItem item;
    item.channel_name = spec.channel;
    item.header = spec.header;
    item.payload_hash = HashBytes(
        reinterpret_cast<const uint8_t*>(spec.payload.data()), spec.payload.size());
    item.payload.assign(spec.payload.begin(), spec.payload.end());
    expected->push_back(std::move(item));
  }
  writer.Close();
  return true;
}

void WriteMixedSourceRecord() {
  record::RecordWriter writer;
  writer.SetSizeOfFileSegmentation(0);
  writer.SetIntervalOfFileSegmentation(0);
  ASSERT_TRUE(writer.Open(kTestMixedSource));

  google::protobuf::FileDescriptorProto image_file;
  image_file.set_name("record_play_test_image.proto");
  image_file.set_package("apollo.drivers");
  auto* image_descriptor = image_file.add_message_type();
  image_descriptor->set_name("Image");
  auto* data_field = image_descriptor->add_field();
  data_field->set_name("data");
  data_field->set_number(1);
  data_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  data_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_BYTES);
  auto* width_field = image_descriptor->add_field();
  width_field->set_name("width");
  width_field->set_number(2);
  width_field->set_label(
      google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  width_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);
  auto* height_field = image_descriptor->add_field();
  height_field->set_name("height");
  height_field->set_number(3);
  height_field->set_label(
      google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  height_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);
  auto* step_field = image_descriptor->add_field();
  step_field->set_name("step");
  step_field->set_number(4);
  step_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  step_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);
  ASSERT_TRUE(message::ProtobufFactory::Instance()->RegisterMessage(image_file));
  std::unique_ptr<google::protobuf::Message> image_message(
      message::ProtobufFactory::Instance()->GenerateMessageByType(
          "apollo.drivers.Image"));
  ASSERT_NE(image_message, nullptr);
  const auto* image_reflection = image_message->GetReflection();
  image_reflection->SetString(
      image_message.get(),
      image_message->GetDescriptor()->FindFieldByName("data"), "image");
  image_reflection->SetUInt32(
      image_message.get(),
      image_message->GetDescriptor()->FindFieldByName("width"), 2);
  image_reflection->SetUInt32(
      image_message.get(),
      image_message->GetDescriptor()->FindFieldByName("height"), 1);
  image_reflection->SetUInt32(
      image_message.get(),
      image_message->GetDescriptor()->FindFieldByName("step"), 5);
  std::string image_bytes;
  ASSERT_TRUE(image_message->SerializeToString(&image_bytes));
  std::string image_descriptor_string;
  message::ProtobufFactory::GetDescriptorString(*image_message,
                                                &image_descriptor_string);

  std::string descriptor;
  message::ProtobufFactory::GetDescriptorString(
      google::protobuf::Any::descriptor(), &descriptor);
  ASSERT_TRUE(writer.WriteChannel("/test/other", "google.protobuf.Any",
                                  descriptor));
  google::protobuf::Any any;
  any.set_type_url("test/type");
  any.set_value("exact protobuf bytes");
  const std::string any_bytes = [&any]() {
    std::string bytes;
    EXPECT_TRUE(any.SerializeToString(&bytes));
    return bytes;
  }();

  const transport::PodChunkHeader cloud_header{
      transport::PodChunkHeader::kMagic,
      transport::PodChunkHeader::kVersion,
      sizeof(transport::PodChunkHeader),
      static_cast<uint32_t>(transport::PodPayloadKind::POINT_CLOUD),
      103,
      2,
      1,
      1,
      24,
      0,
      5,
      9,
      {1, 2, 3, 4}};
  transport::PodMessage cloud(cloud_header, "point", 5);
  std::string cloud_bytes;
  ASSERT_TRUE(cloud.SerializeToString(&cloud_bytes));

  ASSERT_TRUE(writer.WriteChannel(kImageFront6mm, "apollo.drivers.Image",
                                  image_descriptor_string));
  ASSERT_TRUE(writer.WriteChannel(kImageFront12mm, "apollo.drivers.Image",
                                  image_descriptor_string));
  ASSERT_TRUE(writer.WriteChannel(kPointCloud64,
                                  transport::PodMessage::TypeName(),
                                  transport::PodSchemaDescriptor()));
  ASSERT_TRUE(writer.WriteMessage(kImageFront6mm, image_bytes, 101));
  ASSERT_TRUE(writer.WriteMessage("/test/other", any_bytes, 102));
  ASSERT_TRUE(writer.WriteMessage(kPointCloud64, cloud_bytes, 103));
  ASSERT_TRUE(writer.WriteMessage(kImageFront12mm, image_bytes, 104));
  writer.Close();
}

std::string MakeExpectedAnyBytes() {
  google::protobuf::Any any;
  any.set_type_url("test/type");
  any.set_value("exact protobuf bytes");
  std::string bytes;
  EXPECT_TRUE(any.SerializeToString(&bytes));
  return bytes;
}

}  // namespace

TEST(RecordPlayToolTest, ConvertAndBorrowRoundTrip) {
  CleanupArtifacts();
  WriteSourceRecord();

  ConvertedRecordResult result;
  ASSERT_TRUE(ConvertRecordToPod(kTestSource, kTestConverted, kTestManifest,
                                 /*max_per_channel=*/8, &result));
  EXPECT_EQ(result.channels, 3U);
  EXPECT_EQ(result.messages, 9U);
  EXPECT_GT(result.bytes, 0U);

  record::RecordReader converted_reader(kTestConverted);
  ASSERT_TRUE(converted_reader.IsValid());
  for (const auto& channel : converted_reader.GetChannelList()) {
    EXPECT_EQ(converted_reader.GetMessageType(channel),
              transport::PodMessage::TypeName());
    EXPECT_TRUE(transport::IsPodSchemaDescriptor(
        converted_reader.GetProtoDesc(channel)));
  }

  auto pod_stats = BenchmarkPodBorrow(kTestConverted);
  EXPECT_EQ(pod_stats.messages, 9U);
  EXPECT_GT(pod_stats.bytes, 0U);

  ASSERT_TRUE(std::filesystem::exists(kTestManifest));
  ASSERT_TRUE(std::filesystem::exists(kTestConverted));

  CleanupArtifacts();
}

TEST(RecordPlayToolTest, ConvertExistingPodRecordPreservesHeaders) {
  CleanupArtifacts();
  RecordPlayItems expected_items;
  ASSERT_TRUE(WritePodSourceRecord(&expected_items));

  ConvertedRecordResult result;
  ASSERT_TRUE(ConvertRecordToPod(kTestPodSource, kTestConverted, kTestManifest,
                                 /*max_per_channel=*/8, &result));
  EXPECT_EQ(result.channels, 3U);
  EXPECT_EQ(result.messages, expected_items.size());

  std::unordered_map<std::string, RecordPlayItem> expected_by_channel;
  for (const auto& item : expected_items) {
    expected_by_channel.emplace(item.channel_name, item);
  }

  record::RecordReader converted_reader(kTestConverted);
  ASSERT_TRUE(converted_reader.IsValid());
  record::RecordMessage message;
  std::size_t seen = 0;
  while (converted_reader.ReadMessage(&message)) {
    const auto expected_it = expected_by_channel.find(message.channel_name);
    ASSERT_NE(expected_it, expected_by_channel.end());
    transport::PodMessage pod;
    ASSERT_TRUE(pod.ParseFromString(message.content));
    EXPECT_TRUE(ValidateChunk(pod, expected_it->second));
    EXPECT_EQ(message.time, expected_it->second.header.timestamp_ns);
    ++seen;
  }
  EXPECT_EQ(seen, expected_items.size());

  CleanupArtifacts();
}

TEST(RecordPlayToolTest, MixedConversionPreservesNonSensorAndAlignment) {
  CleanupArtifacts();
  WriteMixedSourceRecord();

  MixedConvertedRecordResult result;
  ASSERT_TRUE(ConvertRecordToMixedPod(
      kTestMixedSource, kTestConverted, kTestManifest,
      /*max_per_channel=*/0, &result));
  EXPECT_EQ(result.channels, 4U);
  EXPECT_EQ(result.messages, 4U);
  EXPECT_EQ(result.pod_messages, 3U);
  EXPECT_EQ(result.protobuf_messages, 1U);
  ASSERT_EQ(result.by_type["google.protobuf.Any"].messages, 1U);

  record::RecordReader reader(kTestConverted);
  ASSERT_TRUE(reader.IsValid());
  const std::vector<std::string> expected_channels = {
      kImageFront6mm, "/test/other", kPointCloud64, kImageFront12mm};
  const std::vector<uint64_t> expected_times = {101, 102, 103, 104};
  record::RecordMessage message;
  std::size_t index = 0;
  while (reader.ReadMessage(&message)) {
    ASSERT_LT(index, expected_channels.size());
    EXPECT_EQ(message.channel_name, expected_channels[index]);
    EXPECT_EQ(message.time, expected_times[index]);
    if (message.channel_name == "/test/other") {
      EXPECT_EQ(reader.GetMessageType(message.channel_name),
                "google.protobuf.Any");
      EXPECT_EQ(message.content, MakeExpectedAnyBytes());
    } else {
      EXPECT_EQ(reader.GetMessageType(message.channel_name),
                transport::PodMessage::TypeName());
      transport::PodMessage pod;
      EXPECT_TRUE(pod.ParseFromString(message.content));
    }
    ++index;
  }
  EXPECT_EQ(index, expected_channels.size());
  CleanupArtifacts();
}

TEST(RecordPlayToolTest, ExpandScheduleRepeatZeroUsesSinglePass) {
  RecordPlayItem item_a;
  item_a.channel_name = kImageFront6mm;
  item_a.payload = {1, 2, 3};
  item_a.header.payload_size = static_cast<uint32_t>(item_a.payload.size());

  RecordPlayItem item_b;
  item_b.channel_name = kPointCloud64;
  item_b.payload = {4, 5};
  item_b.header.payload_size = static_cast<uint32_t>(item_b.payload.size());

  const RecordPlayItems items{item_a, item_b};
  const auto schedule = ExpandRecordPlaySchedule(items, /*repeat=*/0);
  ASSERT_EQ(schedule.size(), items.size());
  EXPECT_EQ(schedule[0].channel_name, item_a.channel_name);
  EXPECT_EQ(schedule[1].channel_name, item_b.channel_name);
}

TEST(RecordPlayToolTest, PublishScheduleFailsWhenWriterMissing) {
  RecordPlayItem item;
  item.channel_name = kImageFront6mm;
  item.payload = {9};
  item.header.payload_size = static_cast<uint32_t>(item.payload.size());
  const RecordPlaySchedule schedule{item};

  std::unordered_map<std::string, std::shared_ptr<FakeWriter>> writers;
  EXPECT_FALSE(PublishSchedule(schedule, writers));
}

TEST(RecordPlayToolTest, PublishScheduleStopsOnWriterFailure) {
  RecordPlayItem item;
  item.channel_name = kImageFront6mm;
  item.payload = {9, 8, 7};
  item.header.payload_size = static_cast<uint32_t>(item.payload.size());
  const RecordPlaySchedule schedule{item, item};

  auto writer = std::make_shared<FakeWriter>(false);
  std::unordered_map<std::string, std::shared_ptr<FakeWriter>> writers = {
      {kImageFront6mm, writer},
  };
  EXPECT_FALSE(PublishSchedule(schedule, writers));
  EXPECT_EQ(writer->calls, 1);
}

TEST(RecordPlayToolTest, ValidateChunkDetectsHeaderMismatch) {
  RecordPlayItem item;
  item.channel_name = kImageFront12mm;
  item.header = transport::MakeImagePodChunkHeader(1001, 11, 1280, 720, 2560,
                                                   9, 7, 0x11223344u);
  item.payload = {'a', 'b', 'c'};
  item.payload_hash = HashBytes(
      reinterpret_cast<const uint8_t*>(item.payload.data()), item.payload.size());
  item.header.payload_size = static_cast<uint32_t>(item.payload.size());

  transport::PodMessage message(item.header, item.payload.data(),
                                item.payload.size());
  EXPECT_TRUE(ValidateChunk(message, item));

  RecordPlayItem mismatched = item;
  mismatched.header.timestamp_ns += 1;
  EXPECT_FALSE(ValidateChunk(message, mismatched));
}

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

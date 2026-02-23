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

#include "cyber/message/flatbuffers_message.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "cyber/examples/proto/examples_generated.h"

namespace apollo {
namespace cyber {
namespace message {

static std::vector<uint8_t> BuildChatterBuffer(uint64_t seq,
                                               const char* content) {
  flatbuffers::FlatBufferBuilder fbb;
  auto content_str = fbb.CreateString(content);
  auto chatter = examples::proto::CreateChatter(
      fbb, /*timestamp=*/1000, /*lidar_timestamp=*/2000, seq, content_str);
  fbb.Finish(chatter);
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

TEST(FlatBufferMessageTest, default_constructor) {
  FlatBufferMessage msg;
  EXPECT_EQ(msg.ByteSize(), 0);
  EXPECT_EQ(msg.type_name(), "");
  EXPECT_EQ(msg.timestamp(), 0u);
  EXPECT_EQ(msg.data(), nullptr);
}

TEST(FlatBufferMessageTest, construct_from_buffer) {
  auto buf = BuildChatterBuffer(42, "hello");
  FlatBufferMessage msg("my.Chatter", buf.data(), buf.size());

  EXPECT_EQ(msg.type_name(), "my.Chatter");
  EXPECT_EQ(static_cast<size_t>(msg.ByteSize()), buf.size());
  EXPECT_NE(msg.data(), nullptr);
}

TEST(FlatBufferMessageTest, zero_copy_get_root) {
  auto buf = BuildChatterBuffer(7, "zero-copy");
  FlatBufferMessage msg("my.Chatter", buf.data(), buf.size());

  const auto* chatter = msg.GetRoot<examples::proto::Chatter>();
  ASSERT_NE(chatter, nullptr);
  EXPECT_EQ(chatter->seq(), 7u);
  EXPECT_STREQ(chatter->content()->c_str(), "zero-copy");
  EXPECT_EQ(chatter->timestamp(), 1000u);
  EXPECT_EQ(chatter->lidar_timestamp(), 2000u);
}

TEST(FlatBufferMessageTest, get_root_on_empty_message_returns_null) {
  FlatBufferMessage msg;
  EXPECT_EQ(msg.GetRoot<examples::proto::Chatter>(), nullptr);
}

TEST(FlatBufferMessageTest, serialize_to_array_and_parse_back) {
  auto buf = BuildChatterBuffer(3, "array-roundtrip");
  FlatBufferMessage src("my.Chatter", buf.data(), buf.size());

  std::vector<uint8_t> scratch(src.ByteSize());
  ASSERT_TRUE(
      src.SerializeToArray(scratch.data(), static_cast<int>(scratch.size())));

  FlatBufferMessage dst;
  ASSERT_TRUE(
      dst.ParseFromArray(scratch.data(), static_cast<int>(scratch.size())));

  const auto* chatter = dst.GetRoot<examples::proto::Chatter>();
  ASSERT_NE(chatter, nullptr);
  EXPECT_EQ(chatter->seq(), 3u);
  EXPECT_STREQ(chatter->content()->c_str(), "array-roundtrip");
}

TEST(FlatBufferMessageTest, serialize_to_string_and_parse_back) {
  auto buf = BuildChatterBuffer(9, "string-roundtrip");
  FlatBufferMessage src("my.Chatter", buf.data(), buf.size());

  std::string serialized;
  ASSERT_TRUE(src.SerializeToString(&serialized));
  EXPECT_EQ(serialized.size(), buf.size());

  FlatBufferMessage dst;
  ASSERT_TRUE(dst.ParseFromString(serialized));

  const auto* chatter = dst.GetRoot<examples::proto::Chatter>();
  ASSERT_NE(chatter, nullptr);
  EXPECT_EQ(chatter->seq(), 9u);
  EXPECT_STREQ(chatter->content()->c_str(), "string-roundtrip");
}

TEST(FlatBufferMessageTest, serialize_to_array_null_fails) {
  auto buf = BuildChatterBuffer(1, "x");
  FlatBufferMessage msg("t", buf.data(), buf.size());
  EXPECT_FALSE(msg.SerializeToArray(nullptr, 128));
}

TEST(FlatBufferMessageTest, serialize_to_array_too_small_fails) {
  auto buf = BuildChatterBuffer(1, "x");
  FlatBufferMessage msg("t", buf.data(), buf.size());
  EXPECT_FALSE(msg.SerializeToArray(buf.data(), 0));
}

TEST(FlatBufferMessageTest, serialize_to_string_null_fails) {
  auto buf = BuildChatterBuffer(1, "x");
  FlatBufferMessage msg("t", buf.data(), buf.size());
  EXPECT_FALSE(msg.SerializeToString(nullptr));
}

TEST(FlatBufferMessageTest, parse_from_array_null_fails) {
  FlatBufferMessage msg;
  EXPECT_FALSE(msg.ParseFromArray(nullptr, 8));
}

TEST(FlatBufferMessageTest, parse_from_array_zero_size_fails) {
  FlatBufferMessage msg;
  uint8_t dummy = 0;
  EXPECT_FALSE(msg.ParseFromArray(&dummy, 0));
}

TEST(FlatBufferMessageTest, type_name) {
  EXPECT_EQ(FlatBufferMessage::TypeName(),
            "apollo.cyber.message.FlatBufferMessage");
}

TEST(FlatBufferMessageTest, descriptor) {
  const auto* desc = FlatBufferMessage::descriptor();
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->full_name(), "apollo.cyber.message.FlatBufferMessage");
  EXPECT_EQ(desc->name(), "apollo.cyber.message.FlatBufferMessage");
}

TEST(FlatBufferMessageTest, type_name_setter) {
  FlatBufferMessage msg;
  msg.set_type_name("foo.bar.Baz");
  EXPECT_EQ(msg.type_name(), "foo.bar.Baz");
}

TEST(FlatBufferMessageTest, timestamp_setter) {
  FlatBufferMessage msg;
  msg.set_timestamp(999u);
  EXPECT_EQ(msg.timestamp(), 999u);
}

TEST(FlatBufferMessageTest, copy_constructor) {
  auto buf = BuildChatterBuffer(5, "copy");
  FlatBufferMessage src("my.Chatter", buf.data(), buf.size());
  FlatBufferMessage dst(src);

  EXPECT_EQ(dst.ByteSize(), src.ByteSize());
  EXPECT_EQ(dst.type_name(), src.type_name());

  const auto* chatter = dst.GetRoot<examples::proto::Chatter>();
  ASSERT_NE(chatter, nullptr);
  EXPECT_EQ(chatter->seq(), 5u);
}

TEST(FlatBufferMessageTest, copy_assignment) {
  auto buf = BuildChatterBuffer(6, "assign");
  FlatBufferMessage src("my.Chatter", buf.data(), buf.size());
  FlatBufferMessage dst;
  dst = src;

  const auto* chatter = dst.GetRoot<examples::proto::Chatter>();
  ASSERT_NE(chatter, nullptr);
  EXPECT_EQ(chatter->seq(), 6u);
}

}  // namespace message
}  // namespace cyber
}  // namespace apollo

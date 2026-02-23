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

#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "cyber/examples/proto/examples_generated.h"

namespace apollo {
namespace cyber {
namespace message {

// ---------------------------------------------------------------------------
// Helper: build a Chatter FlatBuffer and return the detached buffer bytes.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> BuildChatterBuffer(uint64_t seq,
                                               const char* content) {
  flatbuffers::FlatBufferBuilder fbb;
  auto content_str = fbb.CreateString(content);
  auto chatter =
      examples::proto::CreateChatter(fbb, /*timestamp=*/1000,
                                     /*lidar_timestamp=*/2000, seq, content_str);
  fbb.Finish(chatter);
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

// ---------------------------------------------------------------------------
// Construction tests
// ---------------------------------------------------------------------------

TEST(FlatBufferMessageTest, DefaultConstructor) {
  FlatBufferMessage msg;
  EXPECT_EQ(msg.ByteSize(), 0);
  EXPECT_EQ(msg.type_name(), "");
  EXPECT_EQ(msg.timestamp(), 0u);
  EXPECT_EQ(msg.data(), nullptr);
}

TEST(FlatBufferMessageTest, ConstructFromBuffer) {
  auto buf = BuildChatterBuffer(42, "hello");
  FlatBufferMessage msg("my.Chatter", buf.data(), buf.size());

  EXPECT_EQ(msg.type_name(), "my.Chatter");
  EXPECT_EQ(static_cast<size_t>(msg.ByteSize()), buf.size());
  EXPECT_NE(msg.data(), nullptr);
}

// ---------------------------------------------------------------------------
// Zero-copy access
// ---------------------------------------------------------------------------

TEST(FlatBufferMessageTest, ZeroCopyGetRoot) {
  auto buf = BuildChatterBuffer(7, "zero-copy");
  FlatBufferMessage msg("my.Chatter", buf.data(), buf.size());

  const auto* chatter = msg.GetRoot<examples::proto::Chatter>();
  ASSERT_NE(chatter, nullptr);
  EXPECT_EQ(chatter->seq(), 7u);
  EXPECT_STREQ(chatter->content()->c_str(), "zero-copy");
  EXPECT_EQ(chatter->timestamp(), 1000u);
  EXPECT_EQ(chatter->lidar_timestamp(), 2000u);
}

TEST(FlatBufferMessageTest, GetRootOnEmptyMessageReturnsNull) {
  FlatBufferMessage msg;
  EXPECT_EQ(msg.GetRoot<examples::proto::Chatter>(), nullptr);
}

// ---------------------------------------------------------------------------
// Serialization round-trip
// ---------------------------------------------------------------------------

TEST(FlatBufferMessageTest, SerializeToArrayAndParseBack) {
  auto buf = BuildChatterBuffer(3, "array-roundtrip");
  FlatBufferMessage src("my.Chatter", buf.data(), buf.size());

  std::vector<uint8_t> scratch(src.ByteSize());
  ASSERT_TRUE(src.SerializeToArray(scratch.data(), static_cast<int>(scratch.size())));

  FlatBufferMessage dst;
  ASSERT_TRUE(dst.ParseFromArray(scratch.data(), static_cast<int>(scratch.size())));

  const auto* chatter = dst.GetRoot<examples::proto::Chatter>();
  ASSERT_NE(chatter, nullptr);
  EXPECT_EQ(chatter->seq(), 3u);
  EXPECT_STREQ(chatter->content()->c_str(), "array-roundtrip");
}

TEST(FlatBufferMessageTest, SerializeToStringAndParseBack) {
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

// ---------------------------------------------------------------------------
// Serialize edge-cases
// ---------------------------------------------------------------------------

TEST(FlatBufferMessageTest, SerializeToArrayNullFails) {
  auto buf = BuildChatterBuffer(1, "x");
  FlatBufferMessage msg("t", buf.data(), buf.size());
  EXPECT_FALSE(msg.SerializeToArray(nullptr, 128));
}

TEST(FlatBufferMessageTest, SerializeToArrayTooSmallFails) {
  auto buf = BuildChatterBuffer(1, "x");
  FlatBufferMessage msg("t", buf.data(), buf.size());
  EXPECT_FALSE(msg.SerializeToArray(buf.data(), 0));
}

TEST(FlatBufferMessageTest, SerializeToStringNullFails) {
  auto buf = BuildChatterBuffer(1, "x");
  FlatBufferMessage msg("t", buf.data(), buf.size());
  EXPECT_FALSE(msg.SerializeToString(nullptr));
}

TEST(FlatBufferMessageTest, ParseFromArrayNullFails) {
  FlatBufferMessage msg;
  EXPECT_FALSE(msg.ParseFromArray(nullptr, 8));
}

TEST(FlatBufferMessageTest, ParseFromArrayZeroSizeFails) {
  FlatBufferMessage msg;
  uint8_t dummy = 0;
  EXPECT_FALSE(msg.ParseFromArray(&dummy, 0));
}

// ---------------------------------------------------------------------------
// Descriptor / TypeName
// ---------------------------------------------------------------------------

TEST(FlatBufferMessageTest, TypeName) {
  EXPECT_EQ(FlatBufferMessage::TypeName(),
            "apollo.cyber.message.FlatBufferMessage");
}

TEST(FlatBufferMessageTest, Descriptor) {
  const auto* desc = FlatBufferMessage::descriptor();
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->full_name(), "apollo.cyber.message.FlatBufferMessage");
  EXPECT_EQ(desc->name(), "apollo.cyber.message.FlatBufferMessage");
}

// ---------------------------------------------------------------------------
// Ancillary fields
// ---------------------------------------------------------------------------

TEST(FlatBufferMessageTest, TypeNameSetter) {
  FlatBufferMessage msg;
  msg.set_type_name("foo.bar.Baz");
  EXPECT_EQ(msg.type_name(), "foo.bar.Baz");
}

TEST(FlatBufferMessageTest, TimestampSetter) {
  FlatBufferMessage msg;
  msg.set_timestamp(999u);
  EXPECT_EQ(msg.timestamp(), 999u);
}

// ---------------------------------------------------------------------------
// Copy semantics
// ---------------------------------------------------------------------------

TEST(FlatBufferMessageTest, CopyConstructor) {
  auto buf = BuildChatterBuffer(5, "copy");
  FlatBufferMessage src("my.Chatter", buf.data(), buf.size());
  FlatBufferMessage dst(src);

  EXPECT_EQ(dst.ByteSize(), src.ByteSize());
  EXPECT_EQ(dst.type_name(), src.type_name());

  const auto* chatter = dst.GetRoot<examples::proto::Chatter>();
  ASSERT_NE(chatter, nullptr);
  EXPECT_EQ(chatter->seq(), 5u);
}

TEST(FlatBufferMessageTest, CopyAssignment) {
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

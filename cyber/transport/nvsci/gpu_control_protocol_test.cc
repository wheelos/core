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

#include "cyber/transport/nvsci/gpu_control_protocol.h"

#include <string>

#include <gtest/gtest.h>

namespace apollo {
namespace cyber {
namespace transport {

namespace {

struct TestMeta {
  uint32_t value = 0;
};

GpuTransportPacket TestPacket() {
  GpuTransportPacket packet;
  packet.channel_id = 7;
  packet.seq_num = 11;
  packet.slot_id = 1;
  packet.prefence.fence_id = 42;
  return packet;
}

}  // namespace

TEST(GpuControlProtocolTest, RoundTripsDataAndAck) {
  const auto packet = TestPacket();
  const std::string metadata(sizeof(TestMeta), '\0');
  auto wire = EncodeGpuDataMessage(packet, metadata);

  GpuTransportPacket decoded;
  std::string decoded_metadata;
  EXPECT_TRUE(DecodeGpuDataMessage(wire, &decoded, &decoded_metadata));
  EXPECT_EQ(decoded.channel_id, packet.channel_id);
  EXPECT_EQ(decoded.seq_num, packet.seq_num);
  EXPECT_EQ(decoded.slot_id, packet.slot_id);
  EXPECT_EQ(decoded.prefence, packet.prefence);
  EXPECT_EQ(decoded_metadata, metadata);

  GpuCompletionPacket ack;
  ack.channel_id = packet.channel_id;
  ack.seq_num = packet.seq_num;
  ack.slot_id = packet.slot_id;
  ack.consumer_id = 99;
  ack.postfence.fence_id = 43;
  auto ack_wire = EncodeGpuAckMessage(ack);
  GpuCompletionPacket decoded_ack;
  EXPECT_TRUE(DecodeGpuAckMessage(ack_wire, &decoded_ack));
  EXPECT_EQ(decoded_ack.consumer_id, ack.consumer_id);
  EXPECT_EQ(decoded_ack.postfence, ack.postfence);
}

TEST(GpuControlProtocolTest, RejectsUnsupportedVersionAndTrailingBytes) {
  auto wire = EncodeGpuDataMessage(TestPacket(), {});
  wire[4] = 2;

  GpuTransportPacket decoded;
  GpuControlDecodeError error = GpuControlDecodeError::kNone;
  EXPECT_FALSE(DecodeGpuDataMessage(wire, &decoded, nullptr, &error));
  EXPECT_EQ(error, GpuControlDecodeError::kUnsupportedVersion);

  wire = EncodeGpuDataMessage(TestPacket(), {});
  wire.push_back('\0');
  EXPECT_FALSE(DecodeGpuDataMessage(wire, &decoded, nullptr, &error));
  EXPECT_EQ(error, GpuControlDecodeError::kInvalidSize);
}

TEST(GpuControlProtocolTest, RejectsInvalidMetadataBoundsAndFence) {
  auto wire = EncodeGpuDataMessage(TestPacket(), {});
  wire[28] = 1;

  GpuTransportPacket decoded;
  GpuControlDecodeError error = GpuControlDecodeError::kNone;
  EXPECT_FALSE(DecodeGpuDataMessage(wire, &decoded, nullptr, &error));
  EXPECT_EQ(error, GpuControlDecodeError::kInvalidSize);

  auto malformed_fence = TestPacket();
  malformed_fence.prefence.fence_id = 0;
  malformed_fence.prefence.timestamp_ns = 1;
  EXPECT_TRUE(EncodeGpuDataMessage(malformed_fence, {}).empty());
}

TEST(GpuControlProtocolTest, UsesStableLittleEndianGoldenEncoding) {
  auto packet = TestPacket();
  packet.timestamp_ns = 0x0102030405060708ULL;
  const std::string encoded = EncodeGpuDataMessage(packet, "xy");

  std::string expected(
      "\x44\x55\x50\x47\x01\x00\x00\x00"
      "\x07\x00\x00\x00\x00\x00\x00\x00"
      "\x0b\x00\x00\x00\x00\x00\x00\x00"
      "\x01\x00\x00\x00\x02\x00\x00\x00"
      "\x08\x07\x06\x05\x04\x03\x02\x01"
      "\x2a\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00",
      56);
  expected.append(48, '\0');
  expected.append("xy");
  EXPECT_EQ(encoded, expected);
  EXPECT_EQ(encoded.size(), kGpuDataHeaderWireSize + 2);

  GpuCompletionPacket ack;
  ack.channel_id = 7;
  ack.seq_num = 11;
  ack.slot_id = 1;
  ack.consumer_id = 0x0102030405060708ULL;
  ack.postfence.fence_id = 43;
  const std::string encoded_ack = EncodeGpuAckMessage(ack);
  EXPECT_EQ(encoded_ack.size(), kGpuAckWireSize);
  EXPECT_EQ(static_cast<uint8_t>(encoded_ack[28]), 0x08);
  EXPECT_EQ(static_cast<uint8_t>(encoded_ack[35]), 0x01);
}

TEST(GpuControlProtocolTest, RoundTripsSessionDescriptor) {
  GpuSessionDescriptor descriptor;
  descriptor.channel_id = 7;
  descriptor.session_id = 9;
  descriptor.config.slot_count = 1;
  descriptor.config.slot_size = 4096;
  descriptor.config.alignment = 256;
  descriptor.backend = GpuBufferBackend::CUDA_IPC;
  descriptor.producer_sync_desc = {1, 2, 3};
  GpuBufferDescriptor buffer;
  buffer.slot_id = 0;
  buffer.capacity = 4096;
  buffer.backend = GpuBufferBackend::CUDA_IPC;
  buffer.nvsci_buf_ipc_desc = {4, 5, 6};
  descriptor.buffers.push_back(buffer);

  const auto wire = EncodeGpuSessionDescriptor(descriptor);
  ASSERT_FALSE(wire.empty());
  EXPECT_EQ(static_cast<uint8_t>(wire[0]), 0x47);
  EXPECT_EQ(static_cast<uint8_t>(wire[1]), 0x50);
  EXPECT_EQ(static_cast<uint8_t>(wire[2]), 0x55);
  EXPECT_EQ(static_cast<uint8_t>(wire[3]), 0x53);

  GpuSessionDescriptor decoded;
  ASSERT_TRUE(DecodeGpuSessionDescriptor(wire, &decoded));
  EXPECT_EQ(decoded.channel_id, descriptor.channel_id);
  EXPECT_EQ(decoded.session_id, descriptor.session_id);
  EXPECT_EQ(decoded.config.slot_size, descriptor.config.slot_size);
  ASSERT_EQ(decoded.buffers.size(), 1U);
  EXPECT_EQ(decoded.buffers[0].nvsci_buf_ipc_desc, buffer.nvsci_buf_ipc_desc);
}

TEST(GpuControlProtocolTest, RoundTripsBootstrapMessages) {
  GpuSessionRequest request;
  request.channel_id = 0x0102030405060708ULL;
  request.consumer_id = 9;
  const std::string request_wire = EncodeGpuSessionRequest(request);
  ASSERT_EQ(request_wire.size(), 24U);
  EXPECT_EQ(static_cast<uint8_t>(request_wire[8]), 0x08);
  EXPECT_EQ(static_cast<uint8_t>(request_wire[15]), 0x01);
  GpuSessionRequest decoded_request;
  ASSERT_TRUE(DecodeGpuSessionRequest(request_wire, &decoded_request));
  EXPECT_EQ(decoded_request.channel_id, request.channel_id);
  EXPECT_EQ(decoded_request.consumer_id, request.consumer_id);

  GpuConsumerRegistration registration;
  registration.channel_id = request.channel_id;
  registration.session_id = 10;
  registration.consumer_id = request.consumer_id;
  registration.consumer_sync_desc = {0xaa, 0xbb};
  const std::string registration_wire =
      EncodeGpuConsumerRegistration(registration);
  GpuConsumerRegistration decoded_registration;
  ASSERT_TRUE(
      DecodeGpuConsumerRegistration(registration_wire, &decoded_registration));
  EXPECT_EQ(decoded_registration.consumer_sync_desc,
            registration.consumer_sync_desc);

  const std::string ack =
      EncodeGpuRegistrationAck(registration.channel_id, registration.session_id,
                               registration.consumer_id);
  uint64_t channel_id = 0;
  uint64_t session_id = 0;
  uint64_t consumer_id = 0;
  ASSERT_TRUE(
      DecodeGpuRegistrationAck(ack, &channel_id, &session_id, &consumer_id));
  EXPECT_EQ(channel_id, registration.channel_id);
  EXPECT_EQ(session_id, registration.session_id);
  EXPECT_EQ(consumer_id, registration.consumer_id);
}

TEST(GpuControlProtocolTest, SerializerRejectsWrongTrivialMetadataSize) {
  TestMeta meta;
  EXPECT_FALSE(GpuMetaSerializer<TestMeta>::Deserialize("x", 1, &meta));
  EXPECT_TRUE(GpuMetaSerializer<TestMeta>::Deserialize(
      reinterpret_cast<const char*>(&meta), sizeof(meta), &meta));
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

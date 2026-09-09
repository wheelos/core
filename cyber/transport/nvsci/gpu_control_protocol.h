/******************************************************************************
 * Copyright 2026 WheelOS. All Rights Reserved.
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

#ifndef CYBER_TRANSPORT_NVSCI_GPU_CONTROL_PROTOCOL_H_
#define CYBER_TRANSPORT_NVSCI_GPU_CONTROL_PROTOCOL_H_

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <google/protobuf/message.h>

#include "cyber/transport/nvsci/gpu_wire.h"
#include "cyber/transport/nvsci/nvsci_types.h"

namespace apollo {
namespace cyber {
namespace transport {

constexpr uint32_t kGpuControlProtocolVersion = 1;
constexpr uint32_t kGpuDataMagic = 0x47505544;             // "GPUD"
constexpr uint32_t kGpuAckMagic = 0x47505541;              // "GPUA"
constexpr uint32_t kGpuSessionMagic = 0x53555047;          // "GPUS"
constexpr uint32_t kGpuSessionRequestMagic = 0x52555047;   // "GPUR"
constexpr uint32_t kGpuRegistrationMagic = 0x47555047;     // "GPUG"
constexpr uint32_t kGpuRegistrationAckMagic = 0x4B555047;  // "GPUK"
constexpr size_t kMaxGpuMetadataSize = 64U * 1024U * 1024U;
constexpr size_t kGpuFenceWireSize = 64;
constexpr size_t kGpuDataHeaderWireSize = 104;
constexpr size_t kGpuAckWireSize = 100;
constexpr size_t kGpuSessionHeaderWireSize = 56;
constexpr size_t kMaxGpuDescriptorSize = 1024U * 1024U;

enum class GpuControlDecodeError {
  kNone = 0,
  kNullOutput,
  kTruncated,
  kBadMagic,
  kUnsupportedVersion,
  kInvalidSize,
  kInvalidFence,
};

inline void SetGpuControlDecodeError(GpuControlDecodeError* error,
                                     GpuControlDecodeError value) {
  if (error != nullptr) {
    *error = value;
  }
}

inline bool IsWellFormedGpuFence(const NvSciSyncFence& fence) {
  if (fence.IsValid()) {
    return true;
  }
  NvSciSyncFence empty;
  return fence == empty;
}

inline void AppendGpuFence(const NvSciSyncFence& fence, std::string* out) {
  wire::AppendU64(fence.fence_id, out);
  wire::AppendU64(fence.timestamp_ns, out);
  out->append(reinterpret_cast<const char*>(fence.payload.data()),
              fence.payload.size());
}

inline bool ReadGpuFence(const uint8_t* data, size_t size, size_t* offset,
                         NvSciSyncFence* fence) {
  if (fence == nullptr ||
      !wire::ReadU64(data, size, offset, &fence->fence_id) ||
      !wire::ReadU64(data, size, offset, &fence->timestamp_ns) ||
      *offset > size || size - *offset < fence->payload.size()) {
    return false;
  }
  std::memcpy(fence->payload.data(), data + *offset, fence->payload.size());
  *offset += fence->payload.size();
  return true;
}

template <typename MetaT, typename Enable = void>
struct GpuMetaSerializer {
  static std::string Serialize(const MetaT& meta) {
    static_assert(std::is_trivially_copyable<MetaT>::value,
                  "MetaT must be trivially copyable or inherit from "
                  "google::protobuf::Message");
    return std::string(reinterpret_cast<const char*>(&meta), sizeof(MetaT));
  }

  static bool Deserialize(const char* data, size_t size, MetaT* meta) {
    if (size != sizeof(MetaT) || meta == nullptr || data == nullptr) {
      return false;
    }
    std::memcpy(meta, data, sizeof(MetaT));
    return true;
  }
};

template <typename MetaT>
struct GpuMetaSerializer<
    MetaT, typename std::enable_if<std::is_base_of<google::protobuf::Message,
                                                   MetaT>::value>::type> {
  static std::string Serialize(const MetaT& meta) {
    std::string s;
    meta.SerializeToString(&s);
    return s;
  }

  static bool Deserialize(const char* data, size_t size, MetaT* meta) {
    if (meta == nullptr || (data == nullptr && size != 0) ||
        size > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    return meta->ParseFromArray(data, static_cast<int>(size));
  }
};

inline std::string EncodeGpuDataMessage(const GpuTransportPacket& packet,
                                        const std::string& meta_bytes) {
  if (meta_bytes.size() > kMaxGpuMetadataSize ||
      meta_bytes.size() > std::numeric_limits<uint32_t>::max() ||
      !IsWellFormedGpuFence(packet.prefence)) {
    return {};
  }
  std::string buf;
  buf.reserve(kGpuDataHeaderWireSize + meta_bytes.size());
  wire::AppendU32(kGpuDataMagic, &buf);
  wire::AppendU32(kGpuControlProtocolVersion, &buf);
  wire::AppendU64(packet.channel_id, &buf);
  wire::AppendU64(packet.seq_num, &buf);
  wire::AppendU32(packet.slot_id, &buf);
  wire::AppendU32(static_cast<uint32_t>(meta_bytes.size()), &buf);
  wire::AppendU64(packet.timestamp_ns, &buf);
  AppendGpuFence(packet.prefence, &buf);
  buf.append(meta_bytes);
  return buf;
}

inline bool DecodeGpuDataMessage(const std::string& raw,
                                 GpuTransportPacket* packet,
                                 std::string* meta_bytes,
                                 GpuControlDecodeError* error = nullptr) {
  SetGpuControlDecodeError(error, GpuControlDecodeError::kNone);
  if (raw.size() < kGpuDataHeaderWireSize || packet == nullptr) {
    SetGpuControlDecodeError(error, packet == nullptr
                                        ? GpuControlDecodeError::kNullOutput
                                        : GpuControlDecodeError::kTruncated);
    return false;
  }
  const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t meta_size = 0;
  if (!wire::ReadU32(data, raw.size(), &offset, &magic) ||
      !wire::ReadU32(data, raw.size(), &offset, &version) ||
      !wire::ReadU64(data, raw.size(), &offset, &packet->channel_id) ||
      !wire::ReadU64(data, raw.size(), &offset, &packet->seq_num) ||
      !wire::ReadU32(data, raw.size(), &offset, &packet->slot_id) ||
      !wire::ReadU32(data, raw.size(), &offset, &meta_size) ||
      !wire::ReadU64(data, raw.size(), &offset, &packet->timestamp_ns) ||
      !ReadGpuFence(data, raw.size(), &offset, &packet->prefence)) {
    SetGpuControlDecodeError(error, GpuControlDecodeError::kTruncated);
    return false;
  }
  if (magic != kGpuDataMagic) {
    SetGpuControlDecodeError(error, GpuControlDecodeError::kBadMagic);
    return false;
  }
  if (version != kGpuControlProtocolVersion) {
    SetGpuControlDecodeError(error, GpuControlDecodeError::kUnsupportedVersion);
    return false;
  }
  if (meta_size > kMaxGpuMetadataSize || meta_size > raw.size() - offset ||
      raw.size() != offset + meta_size) {
    SetGpuControlDecodeError(error, GpuControlDecodeError::kInvalidSize);
    return false;
  }
  if (!IsWellFormedGpuFence(packet->prefence)) {
    SetGpuControlDecodeError(error, GpuControlDecodeError::kInvalidFence);
    return false;
  }
  if (meta_bytes != nullptr) {
    meta_bytes->assign(raw.data() + offset, meta_size);
  }
  return true;
}

inline std::string EncodeGpuAckMessage(const GpuCompletionPacket& ack) {
  if (!IsWellFormedGpuFence(ack.postfence)) {
    return {};
  }
  std::string buf;
  buf.reserve(kGpuAckWireSize);
  wire::AppendU32(kGpuAckMagic, &buf);
  wire::AppendU32(kGpuControlProtocolVersion, &buf);
  wire::AppendU64(ack.channel_id, &buf);
  wire::AppendU64(ack.seq_num, &buf);
  wire::AppendU32(ack.slot_id, &buf);
  wire::AppendU64(ack.consumer_id, &buf);
  AppendGpuFence(ack.postfence, &buf);
  return buf;
}

inline bool DecodeGpuAckMessage(const std::string& raw,
                                GpuCompletionPacket* ack,
                                GpuControlDecodeError* error = nullptr) {
  SetGpuControlDecodeError(error, GpuControlDecodeError::kNone);
  if (raw.size() < kGpuAckWireSize || ack == nullptr) {
    SetGpuControlDecodeError(error, ack == nullptr
                                        ? GpuControlDecodeError::kNullOutput
                                        : GpuControlDecodeError::kTruncated);
    return false;
  }
  const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  if (!wire::ReadU32(data, raw.size(), &offset, &magic) ||
      !wire::ReadU32(data, raw.size(), &offset, &version) ||
      !wire::ReadU64(data, raw.size(), &offset, &ack->channel_id) ||
      !wire::ReadU64(data, raw.size(), &offset, &ack->seq_num) ||
      !wire::ReadU32(data, raw.size(), &offset, &ack->slot_id) ||
      !wire::ReadU64(data, raw.size(), &offset, &ack->consumer_id) ||
      !ReadGpuFence(data, raw.size(), &offset, &ack->postfence)) {
    SetGpuControlDecodeError(error, GpuControlDecodeError::kTruncated);
    return false;
  }
  if (magic != kGpuAckMagic) {
    SetGpuControlDecodeError(error, GpuControlDecodeError::kBadMagic);
    return false;
  }
  if (version != kGpuControlProtocolVersion) {
    SetGpuControlDecodeError(error, GpuControlDecodeError::kUnsupportedVersion);
    return false;
  }
  if (raw.size() != offset || !IsWellFormedGpuFence(ack->postfence)) {
    SetGpuControlDecodeError(error, raw.size() != offset
                                        ? GpuControlDecodeError::kInvalidSize
                                        : GpuControlDecodeError::kInvalidFence);
    return false;
  }
  return true;
}

inline std::string EncodeGpuSessionDescriptor(
    const GpuSessionDescriptor& descriptor) {
  if (descriptor.buffers.empty() ||
      descriptor.buffers.size() > std::numeric_limits<uint32_t>::max() ||
      descriptor.producer_sync_desc.size() > kMaxGpuDescriptorSize) {
    return {};
  }
  std::string out;
  wire::AppendU32(kGpuSessionMagic, &out);
  wire::AppendU32(kGpuControlProtocolVersion, &out);
  wire::AppendU64(descriptor.channel_id, &out);
  wire::AppendU64(descriptor.session_id, &out);
  wire::AppendU32(descriptor.config.slot_count, &out);
  wire::AppendU32(descriptor.config.alignment, &out);
  wire::AppendU64(descriptor.config.slot_size, &out);
  wire::AppendU32(static_cast<uint32_t>(descriptor.backend), &out);
  wire::AppendU32(static_cast<uint32_t>(descriptor.producer_sync_desc.size()),
                  &out);
  wire::AppendU32(static_cast<uint32_t>(descriptor.buffers.size()), &out);
  wire::AppendU32(static_cast<uint32_t>(descriptor.config.access_perm), &out);
  out.append(
      reinterpret_cast<const char*>(descriptor.producer_sync_desc.data()),
      descriptor.producer_sync_desc.size());
  for (const auto& buffer : descriptor.buffers) {
    if (buffer.nvsci_buf_ipc_desc.size() > kMaxGpuDescriptorSize) {
      return {};
    }
    wire::AppendU32(buffer.slot_id, &out);
    wire::AppendU64(buffer.capacity, &out);
    wire::AppendU32(static_cast<uint32_t>(buffer.backend), &out);
    wire::AppendU32(static_cast<uint32_t>(buffer.nvsci_buf_ipc_desc.size()),
                    &out);
    out.append(reinterpret_cast<const char*>(buffer.nvsci_buf_ipc_desc.data()),
               buffer.nvsci_buf_ipc_desc.size());
  }
  return out;
}

inline bool DecodeGpuSessionDescriptor(const std::string& raw,
                                       GpuSessionDescriptor* descriptor) {
  if (descriptor == nullptr || raw.size() < kGpuSessionHeaderWireSize) {
    return false;
  }
  const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t backend = 0;
  uint32_t sync_size = 0;
  uint32_t buffer_count = 0;
  uint32_t access_perm = 0;
  GpuSessionDescriptor parsed;
  if (!wire::ReadU32(data, raw.size(), &offset, &magic) ||
      !wire::ReadU32(data, raw.size(), &offset, &version) ||
      !wire::ReadU64(data, raw.size(), &offset, &parsed.channel_id) ||
      !wire::ReadU64(data, raw.size(), &offset, &parsed.session_id) ||
      !wire::ReadU32(data, raw.size(), &offset, &parsed.config.slot_count) ||
      !wire::ReadU32(data, raw.size(), &offset, &parsed.config.alignment) ||
      !wire::ReadU64(data, raw.size(), &offset, &parsed.config.slot_size) ||
      !wire::ReadU32(data, raw.size(), &offset, &backend) ||
      !wire::ReadU32(data, raw.size(), &offset, &sync_size) ||
      !wire::ReadU32(data, raw.size(), &offset, &buffer_count) ||
      !wire::ReadU32(data, raw.size(), &offset, &access_perm) ||
      magic != kGpuSessionMagic || version != kGpuControlProtocolVersion ||
      parsed.channel_id == 0 || parsed.session_id == 0 || buffer_count == 0 ||
      buffer_count != parsed.config.slot_count ||
      sync_size > kMaxGpuDescriptorSize || offset > raw.size() ||
      raw.size() - offset < sync_size) {
    return false;
  }
  parsed.backend = static_cast<GpuBufferBackend>(backend);
  parsed.config.access_perm = static_cast<NvSciBufAccessPerm>(access_perm);
  parsed.producer_sync_desc.assign(data + offset, data + offset + sync_size);
  offset += sync_size;
  parsed.buffers.reserve(buffer_count);
  for (uint32_t i = 0; i < buffer_count; ++i) {
    GpuBufferDescriptor buffer;
    uint32_t buffer_backend = 0;
    uint32_t descriptor_size = 0;
    if (!wire::ReadU32(data, raw.size(), &offset, &buffer.slot_id) ||
        !wire::ReadU64(data, raw.size(), &offset, &buffer.capacity) ||
        !wire::ReadU32(data, raw.size(), &offset, &buffer_backend) ||
        !wire::ReadU32(data, raw.size(), &offset, &descriptor_size) ||
        descriptor_size > kMaxGpuDescriptorSize || offset > raw.size() ||
        raw.size() - offset < descriptor_size) {
      return false;
    }
    buffer.backend = static_cast<GpuBufferBackend>(buffer_backend);
    buffer.nvsci_buf_ipc_desc.assign(data + offset,
                                     data + offset + descriptor_size);
    offset += descriptor_size;
    parsed.buffers.push_back(std::move(buffer));
  }
  if (offset != raw.size()) {
    return false;
  }
  *descriptor = std::move(parsed);
  return true;
}

struct GpuSessionRequest {
  uint64_t channel_id = 0;
  uint64_t consumer_id = 0;
};

struct GpuConsumerRegistration {
  uint64_t channel_id = 0;
  uint64_t session_id = 0;
  uint64_t consumer_id = 0;
  std::vector<uint8_t> consumer_sync_desc;
};

inline std::string EncodeGpuSessionRequest(const GpuSessionRequest& request) {
  std::string out;
  wire::AppendU32(kGpuSessionRequestMagic, &out);
  wire::AppendU32(kGpuControlProtocolVersion, &out);
  wire::AppendU64(request.channel_id, &out);
  wire::AppendU64(request.consumer_id, &out);
  return out;
}

inline bool DecodeGpuSessionRequest(const std::string& raw,
                                    GpuSessionRequest* request) {
  if (request == nullptr || raw.size() != 24) {
    return false;
  }
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
  return wire::ReadU32(data, raw.size(), &offset, &magic) &&
         wire::ReadU32(data, raw.size(), &offset, &version) &&
         wire::ReadU64(data, raw.size(), &offset, &request->channel_id) &&
         wire::ReadU64(data, raw.size(), &offset, &request->consumer_id) &&
         magic == kGpuSessionRequestMagic &&
         version == kGpuControlProtocolVersion && request->channel_id != 0 &&
         request->consumer_id != 0;
}

inline std::string EncodeGpuConsumerRegistration(
    const GpuConsumerRegistration& registration) {
  if (registration.consumer_sync_desc.empty() ||
      registration.consumer_sync_desc.size() > kMaxGpuDescriptorSize) {
    return {};
  }
  std::string out;
  wire::AppendU32(kGpuRegistrationMagic, &out);
  wire::AppendU32(kGpuControlProtocolVersion, &out);
  wire::AppendU64(registration.channel_id, &out);
  wire::AppendU64(registration.session_id, &out);
  wire::AppendU64(registration.consumer_id, &out);
  wire::AppendU32(static_cast<uint32_t>(registration.consumer_sync_desc.size()),
                  &out);
  wire::AppendU32(0, &out);
  out.append(
      reinterpret_cast<const char*>(registration.consumer_sync_desc.data()),
      registration.consumer_sync_desc.size());
  return out;
}

inline bool DecodeGpuConsumerRegistration(
    const std::string& raw, GpuConsumerRegistration* registration) {
  if (registration == nullptr || raw.size() < 40) {
    return false;
  }
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t sync_size = 0;
  uint32_t reserved = 0;
  const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
  if (!wire::ReadU32(data, raw.size(), &offset, &magic) ||
      !wire::ReadU32(data, raw.size(), &offset, &version) ||
      !wire::ReadU64(data, raw.size(), &offset, &registration->channel_id) ||
      !wire::ReadU64(data, raw.size(), &offset, &registration->session_id) ||
      !wire::ReadU64(data, raw.size(), &offset, &registration->consumer_id) ||
      !wire::ReadU32(data, raw.size(), &offset, &sync_size) ||
      !wire::ReadU32(data, raw.size(), &offset, &reserved) ||
      magic != kGpuRegistrationMagic || version != kGpuControlProtocolVersion ||
      sync_size == 0 || sync_size > kMaxGpuDescriptorSize ||
      raw.size() - offset != sync_size) {
    return false;
  }
  registration->consumer_sync_desc.assign(data + offset,
                                          data + offset + sync_size);
  return true;
}

inline std::string EncodeGpuRegistrationAck(uint64_t channel_id,
                                            uint64_t session_id,
                                            uint64_t consumer_id) {
  std::string out;
  wire::AppendU32(kGpuRegistrationAckMagic, &out);
  wire::AppendU32(kGpuControlProtocolVersion, &out);
  wire::AppendU64(channel_id, &out);
  wire::AppendU64(session_id, &out);
  wire::AppendU64(consumer_id, &out);
  return out;
}

inline bool DecodeGpuRegistrationAck(const std::string& raw,
                                     uint64_t* channel_id, uint64_t* session_id,
                                     uint64_t* consumer_id) {
  if (channel_id == nullptr || session_id == nullptr ||
      consumer_id == nullptr || raw.size() != 32) {
    return false;
  }
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
  return wire::ReadU32(data, raw.size(), &offset, &magic) &&
         wire::ReadU32(data, raw.size(), &offset, &version) &&
         wire::ReadU64(data, raw.size(), &offset, channel_id) &&
         wire::ReadU64(data, raw.size(), &offset, session_id) &&
         wire::ReadU64(data, raw.size(), &offset, consumer_id) &&
         magic == kGpuRegistrationAckMagic &&
         version == kGpuControlProtocolVersion;
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_GPU_CONTROL_PROTOCOL_H_

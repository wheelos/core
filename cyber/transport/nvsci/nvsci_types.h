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

#ifndef CYBER_TRANSPORT_NVSCI_NVSCI_TYPES_H_
#define CYBER_TRANSPORT_NVSCI_NVSCI_TYPES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace apollo {
namespace cyber {
namespace transport {

/**
 * @brief State machine for each buffer slot inside NvSciBufPool.
 */
enum class SlotState : uint8_t {
  FREE = 0,        // Slot is ready to be loaned by producer
  LOANED = 1,      // Slot is borrowed by producer, being written
  IN_USE = 2,      // Slot has been published, consumers are reading
  QUARANTINED = 3  // Slot experienced consumer timeout; isolated until safe
};

/**
 * @brief Memory access permissions for NvSciBuf attribute reconciliation.
 */
enum class NvSciBufAccessPerm : uint8_t {
  READ_ONLY = 1,
  READ_WRITE = 2,
};

/**
 * @brief Hardware synchronization fence.
 *
 * In NVIDIA DriveOS / Jetson Orin NvSciSync, NvSciSyncFence is a 64-byte
 * opaque struct holding hardware semaphore sync point descriptors.
 * This structure maintains 64-byte ABI compatibility while providing
 * high-level helper methods.
 */
struct alignas(8) NvSciSyncFence {
  uint64_t fence_id = 0;
  uint64_t timestamp_ns = 0;
  std::array<uint8_t, 48> payload{};

  bool IsValid() const { return fence_id != 0; }

  void Reset() {
    fence_id = 0;
    timestamp_ns = 0;
    payload.fill(0);
  }

  bool operator==(const NvSciSyncFence& other) const {
    return fence_id == other.fence_id && timestamp_ns == other.timestamp_ns &&
           payload == other.payload;
  }

  bool operator!=(const NvSciSyncFence& other) const {
    return !(*this == other);
  }
};
static_assert(sizeof(NvSciSyncFence) == 64, "NvSciSyncFence must be 64 bytes");

inline bool GetNvSciFenceEngineId(const NvSciSyncFence& fence,
                                  uint64_t* engine_id) {
  if (engine_id == nullptr || fence.payload[0] != 0x53 ||
      fence.payload[1] != 0x59 || fence.payload[2] != 0x4e ||
      fence.payload[3] != 0x43) {
    return false;
  }
  *engine_id = 0;
  for (int i = 0; i < 8; ++i) {
    *engine_id |= static_cast<uint64_t>(fence.payload[8 + i]) << (i * 8);
  }
  return true;
}

/**
 * @brief Underlying buffer sharing backend discriminator.
 */
enum class GpuBufferBackend : uint8_t {
  UNKNOWN = 0,
  CUDA_IPC = 1,
  NVSCI_BUF = 2,
  SIMULATED = 3,
};

constexpr uint32_t kGpuBufferDescriptorVersion = 1;
constexpr uint32_t kCudaIpcBufferMagic = 0x424D5543;    // "CUMB"
constexpr uint32_t kSimulatedBufferMagic = 0x424D4953;  // "SIMB"
constexpr size_t kGpuBufferDescriptorHeaderSize = 24;

/**
 * @brief Descriptor exchanged during one-time channel topology negotiation.
 */
struct GpuBufferDescriptor {
  uint32_t slot_id = 0;
  uint64_t capacity = 0;
  GpuBufferBackend backend = GpuBufferBackend::UNKNOWN;
  std::vector<uint8_t> nvsci_buf_ipc_desc;
};

/**
 * @brief Runtime lightweight zero-copy packet sent from Producer to Consumer.
 *
 * Carries zero raw payload bytes - only the slot index and pre-execution fence.
 */
struct alignas(8) GpuTransportPacket {
  uint64_t channel_id = 0;
  uint32_t slot_id = 0;
  uint64_t seq_num = 0;
  uint64_t timestamp_ns = 0;
  NvSciSyncFence prefence{};
};

/**
 * @brief Completion notification returned from Consumer to Producer.
 */
struct alignas(8) GpuCompletionPacket {
  uint64_t channel_id = 0;
  uint32_t slot_id = 0;
  uint64_t seq_num = 0;
  uint64_t consumer_id = 0;
  NvSciSyncFence postfence{};
};

/**
 * @brief Pool configuration parameters.
 */
struct NvSciBufPoolConfig {
  uint32_t slot_count = 4;
  uint64_t slot_size = 4 * 1024 * 1024;  // Default 4 MiB
  uint32_t alignment = 4096;             // 4 KiB page alignment for GPU MMU
  NvSciBufAccessPerm access_perm = NvSciBufAccessPerm::READ_WRITE;
};

/**
 * @brief Complete one-time cross-process session bootstrap descriptor.
 *
 * The vectors are serialized explicitly by gpu_control_protocol.h; this type
 * is not itself a wire ABI.
 */
struct GpuSessionDescriptor {
  uint64_t channel_id = 0;
  uint64_t session_id = 0;
  NvSciBufPoolConfig config;
  GpuBufferBackend backend = GpuBufferBackend::UNKNOWN;
  std::vector<uint8_t> producer_sync_desc;
  std::vector<GpuBufferDescriptor> buffers;
};

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_NVSCI_TYPES_H_

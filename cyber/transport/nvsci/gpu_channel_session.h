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

#ifndef CYBER_TRANSPORT_NVSCI_GPU_CHANNEL_SESSION_H_
#define CYBER_TRANSPORT_NVSCI_GPU_CHANNEL_SESSION_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cyber/transport/nvsci/nvsci_buf_pool.h"
#include "cyber/transport/nvsci/nvsci_sync_engine.h"
#include "cyber/transport/nvsci/nvsci_types.h"

namespace apollo {
namespace cyber {
namespace transport {

/**
 * @brief Manages 1:N fanout, consumer tracking, slot reference counting
 *        and postfence aggregation for a GPU-GPU zero-copy channel.
 */
class GpuChannelSession {
 public:
  GpuChannelSession(uint64_t channel_id, NvSciBufPoolPtr pool,
                    NvSciSyncEnginePtr sync_engine, uint64_t session_id = 0);
  ~GpuChannelSession() = default;

  uint64_t channel_id() const { return channel_id_; }
  uint64_t session_id() const { return session_id_; }

  int AcquireSlot();
  void* GetDevicePtr(int slot_id) const;

  void RegisterConsumer(uint64_t consumer_id);
  void UnregisterConsumer(uint64_t consumer_id);
  size_t GetConsumerCount() const;

  /**
   * @brief Called by Producer to publish a loaned slot.
   *
   * Automatically initializes refcounts for all currently registered consumers.
   * If require_consumer is true and none are registered, the slot remains
   * loaned and the caller must release it with the producer fence.
   */
  bool OnPublish(int slot_id, const NvSciSyncFence& prefence,
                 GpuTransportPacket* out_packet, bool require_consumer = false);

  /**
   * @brief Rolls back a publish that could not be sent to consumers.
   */
  bool CancelPublish(int slot_id, uint64_t seq_num);

  /**
   * @brief Called when a consumer reports completion of its read.
   */
  bool OnCompletion(const GpuCompletionPacket& packet);

  /**
   * @brief Reclaims slots that have exceeded the timeout waiting for hung
   * consumers.
   *
   * Safely quarantines timed-out slots instead of immediately releasing them,
   * preventing Write-After-Read (WAR) race hazards.
   */
  size_t ReapHungSlots(uint64_t timeout_ns);

  /**
   * @brief Restores quarantined slots back to the active free pool once all
   * associated hardware fences have signaled.
   */
  size_t RecoverQuarantinedSlots();

  void SetQuarantineGracePeriodNs(uint64_t grace_period_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    quarantine_grace_period_ns_ = grace_period_ns;
  }
  uint64_t quarantine_grace_period_ns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quarantine_grace_period_ns_;
  }

  NvSciBufPoolPtr pool() const { return pool_; }
  NvSciSyncEnginePtr sync_engine() const { return sync_engine_; }

 private:
  struct InFlightSlot {
    int slot_id = -1;
    uint64_t seq_num = 0;
    uint64_t publish_timestamp_ns = 0;
    std::unordered_set<uint64_t> pending_consumers;
    NvSciSyncFence last_post_fence{};
    bool has_consumer_post_fence = false;
  };

  struct QuarantinedSlot {
    int slot_id = -1;
    uint64_t seq_num = 0;
    uint64_t quarantine_timestamp_ns = 0;
    std::unordered_set<uint64_t> pending_consumers;
  };

  uint64_t channel_id_ = 0;
  uint64_t session_id_ = 0;
  NvSciBufPoolPtr pool_;
  NvSciSyncEnginePtr sync_engine_;
  mutable std::mutex mutex_;
  std::unordered_set<uint64_t> consumers_;
  std::unordered_map<int, InFlightSlot> in_flight_slots_;
  std::vector<QuarantinedSlot> quarantined_slots_;
  uint64_t quarantine_grace_period_ns_ = 0;
  uint64_t seq_num_ = 0;
};

using GpuChannelSessionPtr = std::shared_ptr<GpuChannelSession>;

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_GPU_CHANNEL_SESSION_H_

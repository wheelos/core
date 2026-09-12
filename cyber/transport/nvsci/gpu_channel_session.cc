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

#include "cyber/transport/nvsci/gpu_channel_session.h"

#include <algorithm>
#include <utility>

namespace apollo {
namespace cyber {
namespace transport {

GpuChannelSession::GpuChannelSession(uint64_t channel_id, NvSciBufPoolPtr pool,
                                     NvSciSyncEnginePtr sync_engine,
                                     uint64_t session_id)
    : channel_id_(channel_id),
      session_id_(session_id),
      pool_(std::move(pool)),
      sync_engine_(std::move(sync_engine)) {}

int GpuChannelSession::AcquireSlot() {
  if (!pool_) {
    return -1;
  }
  for (size_t attempt = 0; attempt < pool_->GetSlotCount(); ++attempt) {
    const int slot_id = pool_->AcquireSlot();
    if (slot_id < 0) {
      return -1;
    }
    bool ready = true;
    if (sync_engine_) {
      for (const auto& fence : pool_->GetLastPostFences(slot_id)) {
        if (fence.IsValid() && !sync_engine_->IsFenceSignaled(fence)) {
          ready = false;
          break;
        }
      }
    }
    if (ready) {
      pool_->ClearPostFences(slot_id);
      return slot_id;
    }
    pool_->ReleaseSlot(slot_id, NvSciSyncFence{});
  }
  return -1;
}

void* GpuChannelSession::GetDevicePtr(int slot_id) const {
  return pool_ ? pool_->GetDevicePtr(slot_id) : nullptr;
}

void GpuChannelSession::RegisterConsumer(uint64_t consumer_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  consumers_.insert(consumer_id);
}

void GpuChannelSession::UnregisterConsumer(uint64_t consumer_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  consumers_.erase(consumer_id);

  // Remove from any in-flight slot expectations and release slot reference
  for (auto it = in_flight_slots_.begin(); it != in_flight_slots_.end();) {
    if (it->second.pending_consumers.erase(consumer_id) > 0) {
      it->second.abandoned_consumer = true;
      if (pool_) {
        pool_->ReleaseSlot(it->first, it->second.last_post_fence);
      }
    }
    if (it->second.pending_consumers.empty()) {
      if (pool_ && it->second.abandoned_consumer &&
          !it->second.has_consumer_post_fence) {
        pool_->ClearPostFences(it->first);
      }
      it = in_flight_slots_.erase(it);
    } else {
      ++it;
    }
  }
}

size_t GpuChannelSession::GetConsumerCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return consumers_.size();
}

bool GpuChannelSession::OnPublish(int slot_id, const NvSciSyncFence& prefence,
                                  GpuTransportPacket* out_packet) {
  if (!pool_ || slot_id < 0 || !out_packet) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

  const int32_t consumer_count = static_cast<int32_t>(consumers_.size());
  // If no consumers registered, mark ref_count = 1 so slot can be released
  // immediately
  const int32_t effective_ref = (consumer_count > 0) ? consumer_count : 1;

  if (!pool_->MarkInUse(slot_id, effective_ref)) {
    return false;
  }

  seq_num_++;
  out_packet->channel_id = channel_id_;
  out_packet->slot_id = static_cast<uint32_t>(slot_id);
  out_packet->seq_num = seq_num_;
  out_packet->timestamp_ns = now_ns;
  out_packet->prefence = prefence;

  if (consumer_count == 0) {
    // No consumer waiting, immediately release the slot back to pool
    pool_->ReleaseSlot(slot_id, prefence);
    return true;
  }

  InFlightSlot flight;
  flight.slot_id = slot_id;
  flight.seq_num = seq_num_;
  flight.publish_timestamp_ns = now_ns;
  flight.pending_consumers = consumers_;
  flight.last_post_fence = prefence;
  in_flight_slots_[slot_id] = std::move(flight);

  return true;
}

bool GpuChannelSession::CancelPublish(int slot_id, uint64_t seq_num) {
  if (!pool_ || slot_id < 0 || seq_num == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = in_flight_slots_.find(slot_id);
  if (it == in_flight_slots_.end() || it->second.seq_num != seq_num) {
    return false;
  }

  const NvSciSyncFence rollback_fence = it->second.last_post_fence;
  for (size_t i = 0; i < it->second.pending_consumers.size(); ++i) {
    pool_->ReleaseSlot(slot_id, rollback_fence);
  }
  in_flight_slots_.erase(it);
  return true;
}

bool GpuChannelSession::OnCompletion(const GpuCompletionPacket& packet) {
  if (!pool_ || packet.channel_id != channel_id_) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const int slot_id = static_cast<int>(packet.slot_id);
  auto it = in_flight_slots_.find(slot_id);
  if (it == in_flight_slots_.end()) {
    return false;
  }

  // Enforce strict non-zero sequence matching to prevent race conditions
  if (packet.seq_num == 0 || it->second.seq_num == 0 ||
      packet.seq_num != it->second.seq_num) {
    return false;
  }

  if (it->second.pending_consumers.erase(packet.consumer_id) == 0) {
    return false;  // Already completed or unknown consumer
  }

  if (packet.postfence.IsValid()) {
    it->second.last_post_fence = packet.postfence;
    it->second.has_consumer_post_fence = true;
  }

  // Release one consumer reference on the underlying pool
  pool_->ReleaseSlot(slot_id, packet.postfence);

  if (it->second.pending_consumers.empty()) {
    in_flight_slots_.erase(it);
  }

  return true;
}

size_t GpuChannelSession::ReapHungSlots(uint64_t timeout_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

  size_t reaped = 0;
  for (auto it = in_flight_slots_.begin(); it != in_flight_slots_.end();) {
    if (now_ns > it->second.publish_timestamp_ns &&
        (now_ns - it->second.publish_timestamp_ns) >= timeout_ns) {
      if (!pool_ || !pool_->QuarantineSlot(it->first)) {
        ++it;
        continue;
      }
      QuarantinedSlot q;
      q.slot_id = it->first;
      q.seq_num = it->second.seq_num;
      q.quarantine_timestamp_ns = now_ns;
      q.last_post_fence = it->second.last_post_fence;
      q.has_consumer_post_fence = it->second.has_consumer_post_fence;
      quarantined_slots_.push_back(std::move(q));

      it = in_flight_slots_.erase(it);
      ++reaped;
    } else {
      ++it;
    }
  }
  return reaped;
}

size_t GpuChannelSession::RecoverQuarantinedSlots() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pool_) {
    return 0;
  }

  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

  size_t recovered = 0;
  for (auto it = quarantined_slots_.begin(); it != quarantined_slots_.end();) {
    bool safe_to_free = false;
    const bool grace_passed =
        (quarantine_grace_period_ns_ == 0) ||
        (now_ns >= it->quarantine_timestamp_ns &&
         (now_ns - it->quarantine_timestamp_ns) >= quarantine_grace_period_ns_);

    if (grace_passed) {
      if (it->has_consumer_post_fence && sync_engine_ &&
          it->last_post_fence.IsValid()) {
        safe_to_free = sync_engine_->IsFenceSignaled(it->last_post_fence);
      } else if (!it->has_consumer_post_fence) {
        if (sync_engine_ && it->last_post_fence.IsValid()) {
          safe_to_free = sync_engine_->IsFenceSignaled(it->last_post_fence);
        } else {
          safe_to_free = true;
        }
      }
    }

    if (safe_to_free) {
      pool_->UnquarantineSlot(it->slot_id);
      it = quarantined_slots_.erase(it);
      ++recovered;
    } else {
      ++it;
    }
  }
  return recovered;
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

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

#ifndef CYBER_TRANSPORT_NVSCI_GPU_LOANED_MESSAGE_H_
#define CYBER_TRANSPORT_NVSCI_GPU_LOANED_MESSAGE_H_

#include <memory>
#include <utility>

#include "cyber/transport/nvsci/gpu_channel_session.h"
#include "cyber/transport/nvsci/nvsci_types.h"

namespace apollo {
namespace cyber {
namespace transport {

/**
 * @brief RAII loaned message for the Producer.
 *
 * Automatically manages slot lifetime, hardware fence generation,
 * and publish notification without CPU-GPU synchronization stalls.
 */
template <typename MetaT>
class GpuLoanedMessage {
 public:
  GpuLoanedMessage() = default;

  GpuLoanedMessage(int slot_id, void* dev_ptr, MetaT* host_meta,
                   std::shared_ptr<GpuChannelSession> session,
                   void* cuda_stream = nullptr)
      : slot_id_(slot_id),
        dev_ptr_(dev_ptr),
        host_meta_(host_meta),
        session_(std::move(session)),
        cuda_stream_(cuda_stream) {
    if (cuda_stream_ != nullptr) {
      WaitOnPreviousPostFences();
    }
  }

  GpuLoanedMessage(MetaT* host_meta,
                   std::shared_ptr<GpuChannelSession> session,
                   void* cuda_stream = nullptr)
      : host_meta_(host_meta),
        session_(std::move(session)),
        cuda_stream_(cuda_stream) {
    if (session_) {
      slot_id_ = session_->AcquireSlot();
      dev_ptr_ = session_->GetDevicePtr(slot_id_);
      if (cuda_stream_ != nullptr) {
        WaitOnPreviousPostFences();
      }
    }
  }

  ~GpuLoanedMessage() {
    if (!published_ && slot_id_ >= 0 && session_ && session_->pool()) {
      // Slot was loaned but never published; cancel and return to pool
      NvSciSyncFence empty_fence{};
      session_->pool()->ReleaseSlot(slot_id_, empty_fence);
    }
  }

  GpuLoanedMessage(const GpuLoanedMessage&) = delete;
  GpuLoanedMessage& operator=(const GpuLoanedMessage&) = delete;

  GpuLoanedMessage(GpuLoanedMessage&& other) noexcept
      : slot_id_(other.slot_id_),
        dev_ptr_(other.dev_ptr_),
        host_meta_(other.host_meta_),
        session_(std::move(other.session_)),
        cuda_stream_(other.cuda_stream_),
        published_(other.published_),
        waited_on_prev_fence_(other.waited_on_prev_fence_),
        wait_succeeded_(other.wait_succeeded_) {
    other.slot_id_ = -1;
    other.dev_ptr_ = nullptr;
    other.host_meta_ = nullptr;
    other.published_ = true;
    other.waited_on_prev_fence_ = true;
    other.wait_succeeded_ = false;
  }

  GpuLoanedMessage& operator=(GpuLoanedMessage&& other) noexcept {
    if (this != &other) {
      if (!published_ && slot_id_ >= 0 && session_ && session_->pool()) {
        NvSciSyncFence empty_fence{};
        session_->pool()->ReleaseSlot(slot_id_, empty_fence);
      }
      slot_id_ = other.slot_id_;
      dev_ptr_ = other.dev_ptr_;
      host_meta_ = other.host_meta_;
      session_ = std::move(other.session_);
      cuda_stream_ = other.cuda_stream_;
      published_ = other.published_;
      waited_on_prev_fence_ = other.waited_on_prev_fence_;
      wait_succeeded_ = other.wait_succeeded_;

      other.slot_id_ = -1;
      other.dev_ptr_ = nullptr;
      other.host_meta_ = nullptr;
      other.published_ = true;
      other.waited_on_prev_fence_ = true;
      other.wait_succeeded_ = false;
    }
    return *this;
  }

  void* device_ptr() const { return dev_ptr_; }
  int slot_id() const { return slot_id_; }
  MetaT* host_meta() const { return host_meta_; }
  MetaT* operator->() const { return host_meta_; }
  MetaT& operator*() const { return *host_meta_; }

  void set_cuda_stream(void* stream) {
    cuda_stream_ = stream;
    if (cuda_stream_ != nullptr) {
      WaitOnPreviousPostFences();
    }
  }
  void* cuda_stream() const { return cuda_stream_; }

  /**
   * @brief Publishes the message.
   *
   * Asynchronously generates a hardware prefence on the associated CUDA stream
   * and dispatches the transport packet to consumers.
   */
  bool Publish(GpuTransportPacket* out_packet = nullptr) {
    if (published_ || slot_id_ < 0 || !session_ || !wait_succeeded_) {
      return false;
    }

    WaitOnPreviousPostFences();
    if (!wait_succeeded_) {
      return false;
    }

    NvSciSyncFence prefence{};
    if (session_->sync_engine()) {
      prefence = session_->sync_engine()->GenerateSignalFence(cuda_stream_);
      if (!prefence.IsValid()) {
        return false;
      }
    }

    GpuTransportPacket local_packet;
    GpuTransportPacket* target_packet = out_packet ? out_packet : &local_packet;
    const bool ok = session_->OnPublish(slot_id_, prefence, target_packet);
    if (ok) {
      published_ = true;
    }
    return ok;
  }

  bool is_valid() const { return slot_id_ >= 0 && dev_ptr_ != nullptr; }

 private:
  void WaitOnPreviousPostFences() {
    if (waited_on_prev_fence_ || slot_id_ < 0 || !session_ || !cuda_stream_) {
      return;
    }
    if (session_->pool() && session_->sync_engine()) {
      auto prev_fences = session_->pool()->GetLastPostFences(slot_id_);
      for (const auto& fence : prev_fences) {
        if (fence.IsValid()) {
          if (!session_->sync_engine()->InsertWaitFence(cuda_stream_, fence)) {
            wait_succeeded_ = false;
            return;
          }
        }
      }
    }
    waited_on_prev_fence_ = true;
  }

  int slot_id_ = -1;
  void* dev_ptr_ = nullptr;
  MetaT* host_meta_ = nullptr;
  std::shared_ptr<GpuChannelSession> session_ = nullptr;
  void* cuda_stream_ = nullptr;
  bool published_ = false;
  bool waited_on_prev_fence_ = false;
  bool wait_succeeded_ = true;
};

/**
 * @brief Const view wrapper for the Consumer.
 *
 * Provides safe access to the received GPU buffer and coordinates
 * hardware wait insertion and completion notification.
 */
template <typename MetaT>
class GpuConstView {
 public:
  GpuConstView(uint64_t channel_id, int slot_id, const void* dev_ptr,
               const MetaT* host_meta, const NvSciSyncFence& prefence,
               uint64_t consumer_id, std::shared_ptr<GpuChannelSession> session,
               uint64_t seq_num)
      : channel_id_(channel_id),
        slot_id_(slot_id),
        dev_ptr_(dev_ptr),
        host_meta_(host_meta),
        prefence_(prefence),
        consumer_id_(consumer_id),
        session_(std::move(session)),
        seq_num_(seq_num) {}

  GpuConstView(const GpuTransportPacket& packet, const MetaT* host_meta,
               uint64_t consumer_id, std::shared_ptr<GpuChannelSession> session)
      : channel_id_(packet.channel_id),
        slot_id_(static_cast<int>(packet.slot_id)),
        dev_ptr_(session && session->pool()
                     ? session->pool()->GetDevicePtr(
                           static_cast<int>(packet.slot_id))
                     : nullptr),
        host_meta_(host_meta),
        prefence_(packet.prefence),
        consumer_id_(consumer_id),
        session_(std::move(session)),
        seq_num_(packet.seq_num) {}

  ~GpuConstView() {
    if (!completed_ && session_) {
      // Auto-signal completion if not manually signaled
      SignalCompletion(nullptr);
    }
  }

  GpuConstView(const GpuConstView&) = delete;
  GpuConstView& operator=(const GpuConstView&) = delete;

  GpuConstView(GpuConstView&& other) noexcept
      : channel_id_(other.channel_id_),
        slot_id_(other.slot_id_),
        dev_ptr_(other.dev_ptr_),
        host_meta_(other.host_meta_),
        prefence_(other.prefence_),
        consumer_id_(other.consumer_id_),
        session_(std::move(other.session_)),
        seq_num_(other.seq_num_),
        completed_(other.completed_) {
    other.completed_ = true;
  }

  GpuConstView& operator=(GpuConstView&& other) noexcept {
    if (this != &other) {
      if (!completed_ && session_) {
        SignalCompletion(nullptr);
      }
      channel_id_ = other.channel_id_;
      slot_id_ = other.slot_id_;
      dev_ptr_ = other.dev_ptr_;
      host_meta_ = other.host_meta_;
      prefence_ = other.prefence_;
      consumer_id_ = other.consumer_id_;
      session_ = std::move(other.session_);
      seq_num_ = other.seq_num_;
      completed_ = other.completed_;
      other.completed_ = true;
    }
    return *this;
  }

  const void* device_ptr() const { return dev_ptr_; }
  int slot_id() const { return slot_id_; }
  uint64_t seq_num() const { return seq_num_; }
  const MetaT* host_meta() const { return host_meta_; }
  const MetaT* operator->() const { return host_meta_; }
  const MetaT& operator*() const { return *host_meta_; }
  const NvSciSyncFence& prefence() const { return prefence_; }

  /**
   * @brief Inserts an asynchronous wait on the consumer's CUDA stream.
   *
   * Downstream kernels will wait for the producer's hardware fence,
   * without CPU blocking.
   */
  bool WaitUntilReady(void* consumer_stream) {
    if (!session_ || !session_->sync_engine()) {
      return false;
    }
    return session_->sync_engine()->InsertWaitFence(consumer_stream, prefence_);
  }

  /**
   * @brief Signals kernel completion by generating a postfence on the stream
   *        and notifying the session.
   */
  bool SignalCompletion(void* consumer_stream) {
    if (completed_ || !session_) {
      return false;
    }

    NvSciSyncFence postfence{};
    if (session_->sync_engine()) {
      postfence = session_->sync_engine()->GenerateSignalFence(consumer_stream);
      if (!postfence.IsValid()) {
        return false;
      }
      if (consumer_stream == nullptr) {
        // Without a stream this is a synchronous CPU-side completion.
        session_->sync_engine()->MarkFenceCompleted(postfence.fence_id);
      }
    }

    GpuCompletionPacket packet;
    packet.channel_id = channel_id_;
    packet.slot_id = static_cast<uint32_t>(slot_id_);
    packet.seq_num = seq_num_;
    packet.consumer_id = consumer_id_;
    packet.postfence = postfence;

    completed_ = true;
    return session_->OnCompletion(packet);
  }

  bool is_completed() const { return completed_; }

 private:
  uint64_t channel_id_ = 0;
  int slot_id_ = -1;
  const void* dev_ptr_ = nullptr;
  const MetaT* host_meta_ = nullptr;
  NvSciSyncFence prefence_{};
  uint64_t consumer_id_ = 0;
  std::shared_ptr<GpuChannelSession> session_ = nullptr;
  uint64_t seq_num_ = 0;
  bool completed_ = false;
};

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_GPU_LOANED_MESSAGE_H_

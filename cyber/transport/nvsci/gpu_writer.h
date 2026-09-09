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

#ifndef CYBER_TRANSPORT_NVSCI_GPU_WRITER_H_
#define CYBER_TRANSPORT_NVSCI_GPU_WRITER_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/common/log.h"
#include "cyber/message/raw_message.h"
#include "cyber/node/node.h"
#include "cyber/node/writer.h"
#include "cyber/transport/nvsci/gpu_channel_manager.h"
#include "cyber/transport/nvsci/gpu_channel_session.h"
#include "cyber/transport/nvsci/gpu_control_protocol.h"
#include "cyber/transport/nvsci/nvsci_types.h"

namespace apollo {
namespace cyber {
namespace transport {

enum class GpuBackpressurePolicy {
  DROP = 0,
  BLOCK = 1,
  TIMEOUT = 2,
};

struct GpuWriterOptions {
  uint32_t slot_count = 4;
  uint64_t slot_size = 4 * 1024 * 1024;  // 4 MiB
  uint32_t alignment = 4096;
  void* stream = nullptr;
  GpuBackpressurePolicy backpressure = GpuBackpressurePolicy::DROP;
  uint64_t timeout_ms = 100;
  uint64_t sync_engine_id = 0;
  uint64_t consumer_lease_timeout_ms = 3000;
  uint64_t quarantine_grace_period_ms = 500;
  uint64_t cleanup_interval_ms = 100;
};

template <typename MetaT>
class GpuWriter;

/**
 * @brief User-facing RAII borrowed GPU buffer for producer write operations.
 */
template <typename MetaT>
class GpuLoan {
 public:
  GpuLoan() = default;

  GpuLoan(int slot_id, void* dev_ptr, size_t capacity,
          std::shared_ptr<GpuChannelSession> session, void* stream)
      : slot_id_(slot_id),
        dev_ptr_(dev_ptr),
        capacity_(capacity),
        session_(std::move(session)),
        stream_(stream) {
    if (stream_ != nullptr) {
      WaitOnPreviousPostFences();
    }
  }

  ~GpuLoan() {
    if (!published_ && slot_id_ >= 0 && session_ && session_->pool()) {
      NvSciSyncFence empty_fence{};
      session_->pool()->ReleaseSlot(slot_id_, empty_fence);
    }
  }

  GpuLoan(const GpuLoan&) = delete;
  GpuLoan& operator=(const GpuLoan&) = delete;

  GpuLoan(GpuLoan&& other) noexcept
      : slot_id_(other.slot_id_),
        dev_ptr_(other.dev_ptr_),
        capacity_(other.capacity_),
        meta_(std::move(other.meta_)),
        session_(std::move(other.session_)),
        stream_(other.stream_),
        published_(other.published_),
        waited_on_prev_fence_(other.waited_on_prev_fence_) {
    other.slot_id_ = -1;
    other.dev_ptr_ = nullptr;
    other.published_ = true;
    other.waited_on_prev_fence_ = true;
  }

  GpuLoan& operator=(GpuLoan&& other) noexcept {
    if (this != &other) {
      if (!published_ && slot_id_ >= 0 && session_ && session_->pool()) {
        NvSciSyncFence empty_fence{};
        session_->pool()->ReleaseSlot(slot_id_, empty_fence);
      }
      slot_id_ = other.slot_id_;
      dev_ptr_ = other.dev_ptr_;
      capacity_ = other.capacity_;
      meta_ = std::move(other.meta_);
      session_ = std::move(other.session_);
      stream_ = other.stream_;
      published_ = other.published_;
      waited_on_prev_fence_ = other.waited_on_prev_fence_;

      other.slot_id_ = -1;
      other.dev_ptr_ = nullptr;
      other.published_ = true;
      other.waited_on_prev_fence_ = true;
    }
    return *this;
  }

  bool is_valid() const { return slot_id_ >= 0 && dev_ptr_ != nullptr; }
  void* device_ptr() const { return dev_ptr_; }
  size_t capacity() const { return capacity_; }
  MetaT& metadata() { return meta_; }
  const MetaT& metadata() const { return meta_; }
  MetaT* operator->() { return &meta_; }
  const MetaT* operator->() const { return &meta_; }
  MetaT& operator*() { return meta_; }
  const MetaT& operator*() const { return meta_; }

  bool WaitOnPreviousPostFences() {
    if (waited_on_prev_fence_ || slot_id_ < 0 || !session_ || !stream_) {
      return true;
    }
    if (session_->pool() && session_->sync_engine()) {
      auto prev_fences = session_->pool()->GetLastPostFences(slot_id_);
      for (const auto& fence : prev_fences) {
        if (fence.IsValid()) {
          if (!session_->sync_engine()->InsertWaitFence(stream_, fence)) {
            return false;
          }
        }
      }
    }
    waited_on_prev_fence_ = true;
    return true;
  }

  void set_stream(void* stream) {
    stream_ = stream;
    if (stream_ != nullptr) {
      WaitOnPreviousPostFences();
    }
  }
  void* stream() const { return stream_; }

 private:
  template <typename>
  friend class GpuWriter;

  int slot_id_ = -1;
  void* dev_ptr_ = nullptr;
  size_t capacity_ = 0;
  MetaT meta_{};
  std::shared_ptr<GpuChannelSession> session_ = nullptr;
  void* stream_ = nullptr;
  bool published_ = false;
  bool waited_on_prev_fence_ = false;
};

/**
 * @brief High-level typed GPU zero-copy Writer integrated with Cyber RT.
 */
template <typename MetaT>
class GpuWriter {
 public:
  GpuWriter(const std::string& channel_name, const GpuWriterOptions& options,
            Node* node)
      : channel_name_(channel_name), options_(options), node_(node) {}

  ~GpuWriter() { Shutdown(); }

  bool Init() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (init_) {
        return true;
      }
    }
    if (!node_) {
      AERROR << "GpuWriter requires a valid Cyber Node";
      return false;
    }

    NvSciBufPoolConfig config;
    config.slot_count = options_.slot_count;
    config.slot_size = options_.slot_size;
    config.alignment = options_.alignment;

    session_ = GpuChannelManager::Instance()->GetOrCreateSession(
        channel_name_, config, options_.sync_engine_id);
    if (!session_) {
      AERROR << "Failed to initialize GpuChannelSession for " << channel_name_;
      return false;
    }
    session_->SetQuarantineGracePeriodNs(
        options_.quarantine_grace_period_ms * 1000000ULL);

    proto::RoleAttributes data_role;
    data_role.set_channel_name(channel_name_ + "/_gpu_data");
    data_role.mutable_qos_profile()->set_depth(
        std::max(64U, options_.slot_count * 16U));
    data_role.mutable_qos_profile()->set_reliability(
        proto::QosReliabilityPolicy::RELIABILITY_RELIABLE);
    data_writer_ = node_->CreateWriter<message::RawMessage>(data_role);
    if (!data_writer_) {
      AERROR << "Failed to create control data writer for " << channel_name_;
      return false;
    }

    ReaderConfig ack_reader_cfg;
    ack_reader_cfg.channel_name = channel_name_ + "/_gpu_ack";
    ack_reader_cfg.pending_queue_size =
        std::max(64U, options_.slot_count * 16U);
    ack_reader_cfg.qos_profile.set_depth(ack_reader_cfg.pending_queue_size);
    ack_reader_ = node_->CreateReader<message::RawMessage>(
        ack_reader_cfg,
        [this](const std::shared_ptr<message::RawMessage>& msg) {
          OnAckReceived(msg);
        });
    if (!ack_reader_) {
      AERROR << "Failed to create control ack reader for " << channel_name_;
      return false;
    }

    GpuSessionDescriptor descriptor;
    if (GpuChannelManager::Instance()->ExportSession(
            channel_name_, &descriptor) == GpuChannelIpcStatus::kSuccess) {
      session_descriptor_wire_ = EncodeGpuSessionDescriptor(descriptor);
    }

    session_writer_ = node_->CreateWriter<message::RawMessage>(channel_name_ +
                                                               "/_gpu_session");
    registration_ack_writer_ = node_->CreateWriter<message::RawMessage>(
        channel_name_ + "/_gpu_registration_ack");
    session_request_reader_ = node_->CreateReader<message::RawMessage>(
        channel_name_ + "/_gpu_session_request",
        [this](const std::shared_ptr<message::RawMessage>& msg) {
          OnSessionRequest(msg);
        });
    registration_reader_ = node_->CreateReader<message::RawMessage>(
        channel_name_ + "/_gpu_registration",
        [this](const std::shared_ptr<message::RawMessage>& msg) {
          OnRegistration(msg);
        });
    if (!session_writer_ || !registration_ack_writer_ ||
        !session_request_reader_ || !registration_reader_) {
      AERROR << "Failed to create GPU session bootstrap endpoints for "
             << channel_name_;
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      init_ = true;
    }
    stop_maintenance_.store(false, std::memory_order_release);
    maintenance_thread_ = std::thread([this]() { MaintenanceLoop(); });
    return true;
  }

  void Shutdown() {
    std::shared_ptr<Writer<message::RawMessage>> data_writer;
    std::shared_ptr<Reader<message::RawMessage>> ack_reader;
    stop_maintenance_.store(true, std::memory_order_release);
    if (maintenance_thread_.joinable()) {
      maintenance_thread_.join();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!init_) {
        return;
      }
      init_ = false;
      data_writer = std::move(data_writer_);
      ack_reader = std::move(ack_reader_);
      session_writer_ = nullptr;
      session_request_reader_ = nullptr;
      registration_reader_ = nullptr;
      registration_ack_writer_ = nullptr;
      remote_consumers_.clear();
      session_ = nullptr;
    }
    GpuChannelManager::Instance()->RemoveSession(channel_name_);
  }

  bool is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return init_ && session_ != nullptr;
  }

  bool HasReader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_writer_ != nullptr && data_writer_->HasReader();
  }

  GpuChannelIpcStatus ExportSessionDescriptors(
      std::vector<GpuBufferDescriptor>* descriptors) const {
    return GpuChannelManager::Instance()->ExportSession(channel_name_,
                                                        descriptors);
  }

  GpuChannelIpcStatus ExportSessionDescriptor(
      GpuSessionDescriptor* descriptor) const {
    return GpuChannelManager::Instance()->ExportSession(channel_name_,
                                                        descriptor);
  }

  bool RegisterRemoteConsumer(uint64_t consumer_id) {
    if (!is_ready() || consumer_id == 0) {
      return false;
    }
    session_->RegisterConsumer(consumer_id);
    return true;
  }

  bool RegisterRemoteConsumer(
      uint64_t consumer_id,
      const std::vector<uint8_t>& consumer_sync_descriptor) {
    if (!is_ready() || consumer_id == 0 || consumer_sync_descriptor.empty() ||
        !session_->sync_engine()->ImportSyncObj(consumer_sync_descriptor)) {
      return false;
    }
    session_->RegisterConsumer(consumer_id);
    return true;
  }

  std::optional<GpuLoan<MetaT>> Loan(void* stream = nullptr) {
    GpuChannelSessionPtr session;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!init_ || !session_) {
        return std::nullopt;
      }
      session = session_;
    }

    if (!session) {
      return std::nullopt;
    }

    int slot = -1;
    if (options_.backpressure == GpuBackpressurePolicy::DROP) {
      slot = session->AcquireSlot();
    } else if (options_.backpressure == GpuBackpressurePolicy::BLOCK) {
      while ((slot = session->AcquireSlot()) < 0) {
        if (!apollo::cyber::OK()) {
          return std::nullopt;
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (!init_) {
            return std::nullopt;
          }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    } else if (options_.backpressure == GpuBackpressurePolicy::TIMEOUT) {
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(options_.timeout_ms);
      while ((slot = session->AcquireSlot()) < 0) {
        if (!apollo::cyber::OK()) {
          return std::nullopt;
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (!init_) {
            return std::nullopt;
          }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }

    if (slot < 0) {
      return std::nullopt;
    }

    void* dev_ptr = session->GetDevicePtr(slot);
    void* active_stream = stream ? stream : options_.stream;
    return GpuLoan<MetaT>(slot, dev_ptr, options_.slot_size, session,
                          active_stream);
  }

  bool Publish(GpuLoan<MetaT>&& loan) {
    GpuChannelSessionPtr session;
    std::shared_ptr<Writer<message::RawMessage>> data_writer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!init_ || !session_ || !data_writer_ || !loan.is_valid()) {
        return false;
      }
      session = session_;
      data_writer = data_writer_;
    }
    if (!session || !data_writer) {
      return false;
    }

    void* stream = loan.stream_ ? loan.stream_ : options_.stream;

    // 1. Enforce Write-After-Read (WAR) hardware hazard protection
    if (!loan.waited_on_prev_fence_) {
      loan.stream_ = stream;
      if (!loan.WaitOnPreviousPostFences()) {
        return false;
      }
    }

    // 2. Generate hardware prefence on the producer stream
    NvSciSyncFence prefence{};
    if (session->sync_engine()) {
      prefence = session->sync_engine()->GenerateSignalFence(stream);
      if (!prefence.IsValid()) {
        return false;
      }
    }

    // 3. Update session in-flight slot tracking
    GpuTransportPacket packet;
    if (!session->OnPublish(loan.slot_id_, prefence, &packet)) {
      return false;
    }

    // 4. Encode and transmit control message over Cyber RT transport
    std::string meta_bytes = GpuMetaSerializer<MetaT>::Serialize(loan.meta_);
    std::string wire_msg = EncodeGpuDataMessage(packet, meta_bytes);
    auto raw_msg = std::make_shared<message::RawMessage>(wire_msg);
    if (!data_writer->Write(raw_msg)) {
      session->CancelPublish(packet.slot_id, packet.seq_num);
      loan.published_ = true;
      return false;
    }

    loan.published_ = true;
    return true;
  }

 private:
  void OnAckReceived(const std::shared_ptr<message::RawMessage>& msg) {
    if (!msg) {
      return;
    }
    GpuChannelSessionPtr session;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      session = session_;
    }
    if (!session) {
      return;
    }
    GpuCompletionPacket ack;
    if (DecodeGpuAckMessage(msg->message, &ack)) {
      session->OnCompletion(ack);
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = remote_consumers_.find(ack.consumer_id);
      if (it != remote_consumers_.end()) {
        it->second = std::chrono::steady_clock::now();
      }
    }
  }

  void OnSessionRequest(const std::shared_ptr<message::RawMessage>& msg) {
    if (!msg || session_descriptor_wire_.empty()) {
      return;
    }
    GpuSessionRequest request;
    if (!DecodeGpuSessionRequest(msg->message, &request)) {
      return;
    }
    GpuChannelSessionPtr session;
    std::shared_ptr<Writer<message::RawMessage>> writer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      session = session_;
      writer = session_writer_;
    }
    if (session && writer && request.channel_id == session->channel_id()) {
      writer->Write(
          std::make_shared<message::RawMessage>(session_descriptor_wire_));
    }
  }

  void OnRegistration(const std::shared_ptr<message::RawMessage>& msg) {
    if (!msg) {
      return;
    }
    GpuConsumerRegistration registration;
    if (!DecodeGpuConsumerRegistration(msg->message, &registration)) {
      return;
    }
    GpuChannelSessionPtr session;
    std::shared_ptr<Writer<message::RawMessage>> ack_writer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      session = session_;
      ack_writer = registration_ack_writer_;
    }
    bool already_imported = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto existing =
          remote_sync_descriptors_.find(registration.consumer_id);
      already_imported = existing != remote_sync_descriptors_.end() &&
                         existing->second == registration.consumer_sync_desc;
    }
    if (!session || registration.channel_id != session->channel_id() ||
        registration.session_id != session->session_id() ||
        (!already_imported && !session->sync_engine()->ImportSyncObj(
                                  registration.consumer_sync_desc))) {
      return;
    }
    session->RegisterConsumer(registration.consumer_id);
    std::vector<uint8_t> old_sync_descriptor;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      remote_consumers_[registration.consumer_id] =
          std::chrono::steady_clock::now();
      const auto old = remote_sync_descriptors_.find(registration.consumer_id);
      if (old != remote_sync_descriptors_.end() &&
          old->second != registration.consumer_sync_desc) {
        old_sync_descriptor = old->second;
      }
      remote_sync_descriptors_[registration.consumer_id] =
          registration.consumer_sync_desc;
    }
    if (!old_sync_descriptor.empty()) {
      uint64_t old_engine_id = 0;
      if (session->sync_engine()->GetSyncObjId(old_sync_descriptor,
                                               &old_engine_id) &&
          session->pool()) {
        session->pool()->ClearPostFencesForEngine(old_engine_id);
      }
      session->sync_engine()->ReleaseSyncObj(old_sync_descriptor);
    }
    if (ack_writer) {
      ack_writer->Write(std::make_shared<message::RawMessage>(
          EncodeGpuRegistrationAck(session->channel_id(), session->session_id(),
                                   registration.consumer_id)));
    }
  }

  void MaintenanceLoop() {
    const auto interval = std::chrono::milliseconds(
        std::max<uint64_t>(1, options_.cleanup_interval_ms));
    while (!stop_maintenance_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(interval);
      GpuChannelSessionPtr session;
      std::vector<uint64_t> expired;
      std::vector<std::vector<uint8_t>> expired_sync_descriptors;
      const auto now = std::chrono::steady_clock::now();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        session = session_;
        for (auto it = remote_consumers_.begin();
             it != remote_consumers_.end();) {
          const auto age =
              std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                    it->second);
          if (age.count() >=
              static_cast<int64_t>(options_.consumer_lease_timeout_ms)) {
            expired.push_back(it->first);
            auto sync_it = remote_sync_descriptors_.find(it->first);
            if (sync_it != remote_sync_descriptors_.end()) {
              expired_sync_descriptors.push_back(std::move(sync_it->second));
              remote_sync_descriptors_.erase(sync_it);
            }
            it = remote_consumers_.erase(it);
          } else {
            ++it;
          }
        }
      }
      if (!session) {
        continue;
      }
      for (uint64_t consumer_id : expired) {
        session->UnregisterConsumer(consumer_id);
      }
      for (const auto& descriptor : expired_sync_descriptors) {
        uint64_t engine_id = 0;
        if (session->sync_engine()->GetSyncObjId(descriptor, &engine_id) &&
            session->pool()) {
          session->pool()->ClearPostFencesForEngine(engine_id);
        }
        session->sync_engine()->ReleaseSyncObj(descriptor);
      }
      session->ReapHungSlots(options_.consumer_lease_timeout_ms * 1000000ULL);
      session->RecoverQuarantinedSlots();
    }
  }

  std::string channel_name_;
  GpuWriterOptions options_;
  Node* node_ = nullptr;
  GpuChannelSessionPtr session_ = nullptr;
  std::shared_ptr<Writer<message::RawMessage>> data_writer_ = nullptr;
  std::shared_ptr<Reader<message::RawMessage>> ack_reader_ = nullptr;
  std::shared_ptr<Writer<message::RawMessage>> session_writer_ = nullptr;
  std::shared_ptr<Reader<message::RawMessage>> session_request_reader_ =
      nullptr;
  std::shared_ptr<Reader<message::RawMessage>> registration_reader_ = nullptr;
  std::shared_ptr<Writer<message::RawMessage>> registration_ack_writer_ =
      nullptr;
  std::string session_descriptor_wire_;
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point>
      remote_consumers_;
  std::unordered_map<uint64_t, std::vector<uint8_t>> remote_sync_descriptors_;
  std::atomic<bool> stop_maintenance_{true};
  std::thread maintenance_thread_;
  mutable std::mutex mutex_;
  bool init_ = false;
};

template <typename MetaT>
std::shared_ptr<GpuWriter<MetaT>> CreateGpuWriter(
    Node* node, const std::string& channel_name,
    const GpuWriterOptions& options = GpuWriterOptions{}) {
  auto writer = std::make_shared<GpuWriter<MetaT>>(channel_name, options, node);
  if (!writer->Init()) {
    return nullptr;
  }
  return writer;
}

template <typename MetaT>
std::shared_ptr<GpuWriter<MetaT>> CreateGpuWriter(
    const std::shared_ptr<Node>& node, const std::string& channel_name,
    const GpuWriterOptions& options = GpuWriterOptions{}) {
  return CreateGpuWriter<MetaT>(node.get(), channel_name, options);
}

template <typename MetaT>
std::shared_ptr<GpuWriter<MetaT>> CreateGpuWriter(
    const std::unique_ptr<Node>& node, const std::string& channel_name,
    const GpuWriterOptions& options = GpuWriterOptions{}) {
  return CreateGpuWriter<MetaT>(node.get(), channel_name, options);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_GPU_WRITER_H_

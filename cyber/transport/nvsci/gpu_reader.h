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

#ifndef CYBER_TRANSPORT_NVSCI_GPU_READER_H_
#define CYBER_TRANSPORT_NVSCI_GPU_READER_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cyber/common/log.h"
#include "cyber/common/util.h"
#include "cyber/message/raw_message.h"
#include "cyber/node/node.h"
#include "cyber/node/reader.h"
#include "cyber/node/writer.h"
#include "cyber/transport/nvsci/gpu_channel_manager.h"
#include "cyber/transport/nvsci/gpu_channel_session.h"
#include "cyber/transport/nvsci/gpu_control_endpoint.h"
#include "cyber/transport/nvsci/gpu_control_protocol.h"
#include "cyber/transport/nvsci/nvsci_types.h"

namespace apollo {
namespace cyber {
namespace transport {

struct GpuReaderOptions {
  void* stream = nullptr;
  uint64_t consumer_id = 0;
  uint32_t slot_count = 4;
  uint64_t slot_size = 4 * 1024 * 1024;
  uint32_t alignment = 4096;
  uint64_t sync_engine_id = 0;
  std::vector<GpuBufferDescriptor> imported_session_descriptors;
  std::vector<uint8_t> imported_producer_sync_desc;
  GpuSessionDescriptor imported_session_descriptor;
  bool automatic_session_bootstrap = true;
  uint64_t bootstrap_timeout_ms = 5000;
  uint64_t heartbeat_interval_ms = 500;
};

template <typename MetaT>
class GpuReader;

/**
 * @brief User-facing const view for received GPU buffers.
 *
 * Automatically handles asynchronous wait insertion and completion postfence
 * signaling on scope exit or explicit Done().
 */
template <typename MetaT>
class GpuMsgView {
 public:
  GpuMsgView(const void* dev_ptr, size_t capacity, const MetaT& meta,
             uint64_t channel_id, int slot_id, uint64_t seq_num,
             uint64_t consumer_id, std::shared_ptr<GpuChannelSession> session,
             std::shared_ptr<Writer<message::RawMessage>> ack_writer,
             void* default_stream,
             const NvSciSyncFence& prefence = NvSciSyncFence{},
             bool waited = false)
      : dev_ptr_(dev_ptr),
        capacity_(capacity),
        meta_(meta),
        channel_id_(channel_id),
        slot_id_(slot_id),
        seq_num_(seq_num),
        consumer_id_(consumer_id),
        session_(std::move(session)),
        ack_writer_(std::move(ack_writer)),
        stream_(default_stream),
        prefence_(prefence),
        waited_(waited) {}

  ~GpuMsgView() {
    if (!completed_ && !Done(stream_)) {
      AERROR << "Failed to complete GPU message for slot " << slot_id_;
    }
  }

  GpuMsgView(const GpuMsgView&) = delete;
  GpuMsgView& operator=(const GpuMsgView&) = delete;

  GpuMsgView(GpuMsgView&& other) noexcept
      : dev_ptr_(other.dev_ptr_),
        capacity_(other.capacity_),
        meta_(std::move(other.meta_)),
        channel_id_(other.channel_id_),
        slot_id_(other.slot_id_),
        seq_num_(other.seq_num_),
        consumer_id_(other.consumer_id_),
        session_(std::move(other.session_)),
        ack_writer_(std::move(other.ack_writer_)),
        stream_(other.stream_),
        prefence_(other.prefence_),
        waited_(other.waited_),
        completed_(other.completed_) {
    other.completed_ = true;
  }

  GpuMsgView& operator=(GpuMsgView&& other) noexcept {
    if (this != &other) {
      if (!completed_) {
        Done(stream_);
      }
      dev_ptr_ = other.dev_ptr_;
      capacity_ = other.capacity_;
      meta_ = std::move(other.meta_);
      channel_id_ = other.channel_id_;
      slot_id_ = other.slot_id_;
      seq_num_ = other.seq_num_;
      consumer_id_ = other.consumer_id_;
      session_ = std::move(other.session_);
      ack_writer_ = std::move(other.ack_writer_);
      stream_ = other.stream_;
      prefence_ = other.prefence_;
      waited_ = other.waited_;
      completed_ = other.completed_;
      other.completed_ = true;
    }
    return *this;
  }

  const void* device_ptr() const { return dev_ptr_; }
  size_t capacity() const { return capacity_; }
  const MetaT& metadata() const { return meta_; }
  const MetaT* operator->() const { return &meta_; }
  const MetaT& operator*() const { return meta_; }

  bool WaitUntilReady(void* consumer_stream) {
    if (waited_ || !prefence_.IsValid() || !session_ ||
        !session_->sync_engine()) {
      return true;
    }
    if (consumer_stream != nullptr) {
      if (!session_->sync_engine()->InsertWaitFence(consumer_stream,
                                                    prefence_)) {
        return false;
      }
      waited_ = true;
      return true;
    }
    return false;
  }

  const NvSciSyncFence& prefence() const { return prefence_; }

  /**
   * @brief Signals kernel completion.
   *
   * Called automatically by destructor, but can also be invoked explicitly
   * with a specific stream.
   */
  bool Done(void* finish_stream = nullptr) {
    if (completed_) {
      return true;
    }
    void* active_stream = finish_stream ? finish_stream : stream_;
    if (active_stream == nullptr || !session_ || !session_->sync_engine()) {
      return false;
    }
    NvSciSyncFence postfence{};
    postfence = session_->sync_engine()->GenerateSignalFence(active_stream);
    if (!postfence.IsValid()) {
      return false;
    }

    GpuCompletionPacket ack;
    ack.channel_id = channel_id_;
    ack.slot_id = static_cast<uint32_t>(slot_id_);
    ack.seq_num = seq_num_;
    ack.consumer_id = consumer_id_;
    ack.postfence = postfence;

    const bool completed_locally = session_->OnCompletion(ack);
    bool ack_sent = false;
    if (ack_writer_) {
      std::string ack_wire = EncodeGpuAckMessage(ack);
      ack_sent = !ack_wire.empty() &&
                 ack_writer_->Write(std::make_shared<message::RawMessage>(
                     std::move(ack_wire)));
    }
    completed_ = completed_locally || ack_sent;
    return completed_;
  }

 private:
  const void* dev_ptr_ = nullptr;
  size_t capacity_ = 0;
  MetaT meta_{};
  uint64_t channel_id_ = 0;
  int slot_id_ = -1;
  uint64_t seq_num_ = 0;
  uint64_t consumer_id_ = 0;
  std::shared_ptr<GpuChannelSession> session_ = nullptr;
  std::shared_ptr<Writer<message::RawMessage>> ack_writer_ = nullptr;
  void* stream_ = nullptr;
  NvSciSyncFence prefence_{};
  bool waited_ = false;
  bool completed_ = false;
};

/**
 * @brief High-level typed GPU zero-copy Reader integrated with Cyber RT.
 */
template <typename MetaT>
class GpuReader {
 public:
  using CallbackFunc = std::function<void(GpuMsgView<MetaT>&)>;

  GpuReader(const std::string& channel_name, const GpuReaderOptions& options,
            Node* node, CallbackFunc callback)
      : channel_name_(channel_name),
        options_(options),
        node_(node),
        callback_(std::move(callback)) {
    if (options_.consumer_id == 0) {
      options_.consumer_id = NewGpuEndpointId();
    }
    if (options_.sync_engine_id == 0) {
      static std::atomic<uint64_t> next_engine{1};
      const uint64_t clock_bits = static_cast<uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count());
      options_.sync_engine_id =
          clock_bits ^ (options_.consumer_id << 17) ^
          next_engine.fetch_add(1, std::memory_order_relaxed);
      if (options_.sync_engine_id == 0) {
        options_.sync_engine_id = 1;
      }
    }
  }

  ~GpuReader() { Shutdown(); }

  bool Init() {
    if (init_) {
      return true;
    }
    if (!node_) {
      AERROR << "GpuReader requires a valid Cyber Node";
      return false;
    }
    if (options_.stream == nullptr) {
      AERROR << "GpuReader requires an explicit CUDA stream";
      return false;
    }

    NvSciBufPoolConfig config;
    config.slot_count = options_.slot_count;
    config.slot_size = options_.slot_size;
    config.alignment = options_.alignment;
    if (options_.imported_session_descriptor.session_id != 0) {
      const auto status = GpuChannelManager::Instance()->CreateImportedSession(
          channel_name_, options_.imported_session_descriptor,
          options_.sync_engine_id, &session_);
      if (status != GpuChannelIpcStatus::kSuccess) {
        return false;
      }
      remote_session_ = true;
    } else if (!options_.imported_session_descriptors.empty()) {
      const auto status = GpuChannelManager::Instance()->CreateImportedSession(
          channel_name_, config, options_.imported_session_descriptors,
          options_.sync_engine_id, options_.imported_producer_sync_desc,
          &session_);
      if (status != GpuChannelIpcStatus::kSuccess) {
        AERROR << "Failed to import GPU session for " << channel_name_
               << " (status " << static_cast<int>(status) << ")";
        return false;
      }
      remote_session_ = true;
    } else {
      session_ = GpuChannelManager::Instance()->GetSession(channel_name_);
      if (!session_ && options_.automatic_session_bootstrap) {
        if (!BootstrapRemoteSession()) {
          AERROR << "Timed out bootstrapping GPU session for " << channel_name_;
          return false;
        }
        remote_session_ = true;
        bootstrap_managed_ = true;
      } else if (!session_) {
        session_ = GpuChannelManager::Instance()->GetOrCreateSession(
            channel_name_, config, options_.sync_engine_id);
      }
    }
    if (!session_) {
      AERROR << "Failed to locate GpuChannelSession for " << channel_name_;
      return false;
    }

    if (bootstrap_managed_) {
      if (!RegisterRemoteSession()) {
        AERROR << "Failed to register remote GPU consumer for "
               << channel_name_;
        return false;
      }
    } else {
      session_->RegisterConsumer(options_.consumer_id);
    }

    proto::RoleAttributes ack_role;
    ack_role.set_channel_name(channel_name_ + "/_gpu_ack");
    ack_role.mutable_qos_profile()->set_depth(
        std::max(64U, options_.slot_count * 16U));
    ack_role.mutable_qos_profile()->set_reliability(
        proto::QosReliabilityPolicy::RELIABILITY_RELIABLE);
    ack_writer_ = node_->CreateWriter<message::RawMessage>(ack_role);
    if (!ack_writer_) {
      AERROR << "Failed to create control ack writer for " << channel_name_;
      return false;
    }

    ReaderConfig data_reader_cfg;
    data_reader_cfg.channel_name = channel_name_ + "/_gpu_data";
    data_reader_cfg.pending_queue_size =
        std::max(64U, options_.slot_count * 16U);
    data_reader_cfg.qos_profile.set_depth(data_reader_cfg.pending_queue_size);
    data_reader_ = GpuControlDispatcher::Instance()->Subscribe(
        node_, data_reader_cfg, options_.consumer_id,
        [this](const std::shared_ptr<message::RawMessage>& msg) {
          OnDataReceived(msg);
        });
    if (!data_reader_) {
      AERROR << "Failed to create control data reader for " << channel_name_;
      return false;
    }

    init_ = true;
    if (bootstrap_managed_) {
      stop_heartbeat_.store(false, std::memory_order_release);
      heartbeat_thread_ = std::thread([this]() { HeartbeatLoop(); });
    }
    return true;
  }

  void Shutdown() {
    init_ = false;
    stop_heartbeat_.store(true, std::memory_order_release);
    bootstrap_cv_.notify_all();
    if (heartbeat_thread_.joinable()) {
      heartbeat_thread_.join();
    }
    GpuChannelSessionPtr session;
    {
      std::lock_guard<std::mutex> lock(bootstrap_mutex_);
      session = std::move(session_);
    }
    if (session) {
      if (!remote_session_) {
        session->UnregisterConsumer(options_.consumer_id);
      } else if (registration_writer_) {
        // Send explicit unregistration message to writer
        GpuConsumerUnregister unreg;
        unreg.channel_id = session->channel_id();
        unreg.session_id = session->session_id();
        unreg.consumer_id = options_.consumer_id;
        std::string unreg_wire = EncodeGpuConsumerUnregister(unreg);
        if (!unreg_wire.empty()) {
          registration_writer_->Write(
              std::make_shared<message::RawMessage>(unreg_wire));
        }
      }
    }
    data_reader_ = nullptr;
    ack_writer_ = nullptr;
    session_request_writer_ = nullptr;
    session_reader_ = nullptr;
    registration_writer_ = nullptr;
    registration_ack_reader_ = nullptr;
  }

  bool is_ready() const {
    std::lock_guard<std::mutex> lock(bootstrap_mutex_);
    return init_ && session_ != nullptr;
  }
  uint64_t consumer_id() const { return options_.consumer_id; }
  bool HasWriter() const {
    return data_reader_ != nullptr && data_reader_->HasWriter();
  }

  bool ExportSyncDescriptor(std::vector<uint8_t>* descriptor) const {
    return session_ && session_->sync_engine() &&
           session_->sync_engine()->ExportSyncObj(descriptor);
  }

 private:
  bool BootstrapRemoteSession() {
    session_request_writer_ = node_->CreateWriter<message::RawMessage>(
        channel_name_ + "/_gpu_session_request");
    session_reader_ = GpuControlDispatcher::Instance()->Subscribe(
        node_, channel_name_ + "/_gpu_session", options_.consumer_id,
        [this](const std::shared_ptr<message::RawMessage>& msg) {
          OnSessionDescriptor(msg);
        });
    registration_writer_ = node_->CreateWriter<message::RawMessage>(
        channel_name_ + "/_gpu_registration");
    registration_ack_reader_ = GpuControlDispatcher::Instance()->Subscribe(
        node_, channel_name_ + "/_gpu_registration_ack", options_.consumer_id,
        [this](const std::shared_ptr<message::RawMessage>& msg) {
          OnRegistrationAck(msg);
        });
    if (!session_request_writer_ || !session_reader_ || !registration_writer_ ||
        !registration_ack_reader_) {
      return false;
    }

    GpuSessionRequest request;
    request.channel_id = static_cast<uint64_t>(common::Hash(channel_name_));
    request.consumer_id = options_.consumer_id;
    session_request_wire_ = EncodeGpuSessionRequest(request);
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options_.bootstrap_timeout_ms);
    std::unique_lock<std::mutex> lock(bootstrap_mutex_);
    while (!session_) {
      lock.unlock();
      session_request_writer_->Write(
          std::make_shared<message::RawMessage>(session_request_wire_));
      lock.lock();
      if (bootstrap_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                 [this]() { return session_ != nullptr; })) {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
    }
    return session_ != nullptr;
  }

  void OnSessionDescriptor(const std::shared_ptr<message::RawMessage>& msg) {
    if (!msg) {
      return;
    }
    GpuSessionDescriptor descriptor;
    if (!DecodeGpuSessionDescriptor(msg->message, &descriptor) ||
        descriptor.channel_id !=
            static_cast<uint64_t>(common::Hash(channel_name_))) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(bootstrap_mutex_);
      if (session_ != nullptr &&
          session_->session_id() == descriptor.session_id &&
          registration_confirmed_) {
        return;
      }
    }
    GpuChannelSessionPtr imported_session;
    const auto status = GpuChannelManager::Instance()->CreateImportedSession(
        channel_name_, descriptor, options_.sync_engine_id, &imported_session);
    if (status != GpuChannelIpcStatus::kSuccess) {
      return;
    }
    std::string registration_wire;
    if (imported_session && imported_session->sync_engine()) {
      std::vector<uint8_t> sync_desc;
      if (imported_session->sync_engine()->ExportSyncObj(&sync_desc)) {
        GpuConsumerRegistration registration;
        registration.channel_id = imported_session->channel_id();
        registration.session_id = imported_session->session_id();
        registration.consumer_id = options_.consumer_id;
        registration.consumer_sync_desc = std::move(sync_desc);
        registration_wire = EncodeGpuConsumerRegistration(registration);
      }
    }
    {
      std::lock_guard<std::mutex> lock(bootstrap_mutex_);
      session_ = std::move(imported_session);
      registration_wire_ = std::move(registration_wire);
      registration_confirmed_ = false;
    }
    bootstrap_cv_.notify_all();
  }

  bool RegisterRemoteSession() {
    std::vector<uint8_t> sync_desc;
    if (!session_ || !session_->sync_engine() ||
        !session_->sync_engine()->ExportSyncObj(&sync_desc)) {
      return false;
    }
    GpuConsumerRegistration registration;
    registration.channel_id = session_->channel_id();
    registration.session_id = session_->session_id();
    registration.consumer_id = options_.consumer_id;
    registration.consumer_sync_desc = std::move(sync_desc);
    registration_wire_ = EncodeGpuConsumerRegistration(registration);
    if (registration_wire_.empty()) {
      return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options_.bootstrap_timeout_ms);
    std::unique_lock<std::mutex> lock(bootstrap_mutex_);
    while (!registration_confirmed_) {
      lock.unlock();
      registration_writer_->Write(
          std::make_shared<message::RawMessage>(registration_wire_));
      lock.lock();
      if (bootstrap_cv_.wait_for(
              lock, std::chrono::milliseconds(100),
              [this]() { return registration_confirmed_; })) {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
    }
    return registration_confirmed_;
  }

  void OnRegistrationAck(const std::shared_ptr<message::RawMessage>& msg) {
    if (!msg) {
      return;
    }
    uint64_t channel_id = 0;
    uint64_t session_id = 0;
    uint64_t consumer_id = 0;
    if (!DecodeGpuRegistrationAck(msg->message, &channel_id, &session_id,
                                  &consumer_id)) {
      return;
    }
    std::lock_guard<std::mutex> lock(bootstrap_mutex_);
    if (session_ && channel_id == session_->channel_id() &&
        session_id == session_->session_id() &&
        consumer_id == options_.consumer_id) {
      registration_confirmed_ = true;
      bootstrap_cv_.notify_all();
    }
  }

  void HeartbeatLoop() {
    const auto interval = std::chrono::milliseconds(
        std::max<uint64_t>(1, options_.heartbeat_interval_ms));
    while (!stop_heartbeat_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(interval);
      if (stop_heartbeat_.load(std::memory_order_acquire)) {
        break;
      }
      std::string registration_wire;
      std::string request_wire;
      bool need_request = false;
      {
        std::lock_guard<std::mutex> lock(bootstrap_mutex_);
        registration_wire = registration_wire_;
        need_request = (session_ == nullptr || !registration_confirmed_);
        if (need_request) {
          request_wire = session_request_wire_;
        }
      }
      if (registration_writer_ && !registration_wire.empty()) {
        registration_writer_->Write(
            std::make_shared<message::RawMessage>(registration_wire));
      }
      if (session_request_writer_ && need_request && !request_wire.empty()) {
        session_request_writer_->Write(
            std::make_shared<message::RawMessage>(request_wire));
      }
    }
  }

  void OnDataReceived(const std::shared_ptr<message::RawMessage>& msg) {
    GpuChannelSessionPtr session;
    {
      std::lock_guard<std::mutex> lock(bootstrap_mutex_);
      session = session_;
    }
    if (!msg || !session || !callback_) {
      return;
    }

    GpuTransportPacket packet;
    std::string meta_bytes;
    GpuControlDecodeError decode_error = GpuControlDecodeError::kNone;
    if (!DecodeGpuDataMessage(msg->message, &packet, &meta_bytes,
                              &decode_error)) {
      AERROR << "Rejected malformed GPU data control message on "
             << channel_name_ << " (error " << static_cast<int>(decode_error)
             << ")";
      return;
    }
    if (packet.channel_id != session->channel_id() || packet.seq_num == 0) {
      AERROR
          << "Rejected GPU data control message with invalid packet identity "
          << "on " << channel_name_;
      return;
    }
    if (!session->pool() || packet.slot_id >= session->pool()->GetSlotCount()) {
      AERROR << "Rejected GPU data control message with out-of-range slot "
             << packet.slot_id << " on " << channel_name_;
      return;
    }

    void* dev_ptr = session->GetDevicePtr(static_cast<int>(packet.slot_id));
    if (dev_ptr == nullptr) {
      return;
    }

    // 1. Insert hardware wait fence asynchronously on consumer CUDA stream
    bool waited = false;
    if (session->sync_engine() && packet.prefence.IsValid()) {
      if (!session->sync_engine()->InsertWaitFence(options_.stream,
                                                   packet.prefence)) {
        AERROR << "Failed to insert GPU prefence on " << channel_name_;
        return;
      }
      waited = true;
    }

    // 2. Deserialize metadata
    MetaT meta{};
    if (!GpuMetaSerializer<MetaT>::Deserialize(meta_bytes.data(),
                                               meta_bytes.size(), &meta)) {
      AERROR << "Rejected GPU data control message with malformed metadata on "
             << channel_name_;
      return;
    }

    // 3. Construct view and invoke user callback
    size_t capacity = session->pool() ? session->pool()->GetSlotCapacity() : 0;
    GpuMsgView<MetaT> view(dev_ptr, capacity, meta, packet.channel_id,
                           static_cast<int>(packet.slot_id), packet.seq_num,
                           options_.consumer_id, session, ack_writer_,
                           options_.stream, packet.prefence, waited);

    callback_(view);
    // Destructor of view automatically generates postfence and emits ACK
  }

  std::string channel_name_;
  GpuReaderOptions options_;
  Node* node_ = nullptr;
  CallbackFunc callback_ = nullptr;
  GpuChannelSessionPtr session_ = nullptr;
  std::shared_ptr<GpuControlSubscription> data_reader_ = nullptr;
  std::shared_ptr<Writer<message::RawMessage>> ack_writer_ = nullptr;
  std::shared_ptr<Writer<message::RawMessage>> session_request_writer_ =
      nullptr;
  std::shared_ptr<GpuControlSubscription> session_reader_ = nullptr;
  std::shared_ptr<Writer<message::RawMessage>> registration_writer_ = nullptr;
  std::shared_ptr<GpuControlSubscription> registration_ack_reader_ = nullptr;
  mutable std::mutex bootstrap_mutex_;
  std::condition_variable bootstrap_cv_;
  std::string registration_wire_;
  std::string session_request_wire_;
  std::atomic<bool> stop_heartbeat_{true};
  std::thread heartbeat_thread_;
  bool registration_confirmed_ = false;
  bool remote_session_ = false;
  bool bootstrap_managed_ = false;
  std::atomic<bool> init_{false};
};

template <typename MetaT>
std::shared_ptr<GpuReader<MetaT>> CreateGpuReader(
    Node* node, const std::string& channel_name,
    const GpuReaderOptions& options,
    std::function<void(GpuMsgView<MetaT>&)> callback) {
  auto reader = std::make_shared<GpuReader<MetaT>>(channel_name, options, node,
                                                   std::move(callback));
  if (!reader->Init()) {
    return nullptr;
  }
  return reader;
}

template <typename MetaT>
std::shared_ptr<GpuReader<MetaT>> CreateGpuReader(
    const std::shared_ptr<Node>& node, const std::string& channel_name,
    const GpuReaderOptions& options,
    std::function<void(GpuMsgView<MetaT>&)> callback) {
  return CreateGpuReader<MetaT>(node.get(), channel_name, options,
                                std::move(callback));
}

template <typename MetaT>
std::shared_ptr<GpuReader<MetaT>> CreateGpuReader(
    const std::unique_ptr<Node>& node, const std::string& channel_name,
    const GpuReaderOptions& options,
    std::function<void(GpuMsgView<MetaT>&)> callback) {
  return CreateGpuReader<MetaT>(node.get(), channel_name, options,
                                std::move(callback));
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_GPU_READER_H_

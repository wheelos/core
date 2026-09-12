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

#ifndef CYBER_TRANSPORT_NVSCI_GPU_CONTROL_ENDPOINT_H_
#define CYBER_TRANSPORT_NVSCI_GPU_CONTROL_ENDPOINT_H_

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/message/message_traits.h"
#include "cyber/message/protobuf_factory.h"
#include "cyber/message/raw_message.h"
#include "cyber/node/node.h"
#include "cyber/node/reader.h"
#include "cyber/transport/common/identity.h"

namespace apollo {
namespace cyber {
namespace transport {

inline uint64_t NewGpuEndpointId() {
  const uint64_t id = Identity().HashValue();
  return id == 0 ? 1 : id;
}

class GpuControlDispatcher;

class GpuControlSubscription {
 public:
  ~GpuControlSubscription();

  bool HasWriter() const;

  GpuControlSubscription(const GpuControlSubscription&) = delete;
  GpuControlSubscription& operator=(const GpuControlSubscription&) = delete;

 private:
  friend class GpuControlDispatcher;
  GpuControlSubscription(std::string channel_name, uint64_t endpoint_id)
      : channel_name_(std::move(channel_name)), endpoint_id_(endpoint_id) {}

  std::string channel_name_;
  uint64_t endpoint_id_ = 0;
};

class GpuControlDispatcher {
 public:
  using Callback = CallbackFunc<message::RawMessage>;

  static GpuControlDispatcher* Instance() {
    static GpuControlDispatcher dispatcher;
    return &dispatcher;
  }

  std::shared_ptr<GpuControlSubscription> Subscribe(Node* node,
                                                    const ReaderConfig& config,
                                                    uint64_t endpoint_id,
                                                    Callback callback) {
    if (node == nullptr || config.channel_name.empty() || endpoint_id == 0 ||
        callback == nullptr) {
      return nullptr;
    }

    std::shared_ptr<ChannelEntry> entry;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto& existing = channels_[config.channel_name];
      if (!existing) {
        existing = std::make_shared<ChannelEntry>();
        existing->reader = CreateReader(node, config, endpoint_id, existing);
        if (!existing->reader) {
          channels_.erase(config.channel_name);
          return nullptr;
        }
      }
      entry = existing;
    }

    auto subscriber = std::make_shared<Subscriber>();
    subscriber->callback = std::move(callback);
    subscriber->queue_capacity =
        std::max<size_t>(1, static_cast<size_t>(config.pending_queue_size));
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      if (!entry->subscribers.emplace(endpoint_id, subscriber).second) {
        return nullptr;
      }
    }
    subscriber->worker =
        std::thread([subscriber]() { RunSubscriber(subscriber); });
    return std::shared_ptr<GpuControlSubscription>(
        new GpuControlSubscription(config.channel_name, endpoint_id));
  }

  std::shared_ptr<GpuControlSubscription> Subscribe(
      Node* node, const std::string& channel_name, uint64_t endpoint_id,
      Callback callback, uint32_t pending_queue_size = 1) {
    ReaderConfig config;
    config.channel_name = channel_name;
    config.pending_queue_size = std::max(1U, pending_queue_size);
    config.qos_profile.set_depth(config.pending_queue_size);
    return Subscribe(node, config, endpoint_id, std::move(callback));
  }

  void Unsubscribe(const std::string& channel_name, uint64_t endpoint_id) {
    std::shared_ptr<ChannelEntry> entry;
    std::shared_ptr<Subscriber> subscriber;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto channel = channels_.find(channel_name);
      if (channel == channels_.end()) {
        return;
      }
      entry = channel->second;
      {
        std::lock_guard<std::mutex> entry_lock(entry->mutex);
        const auto current = entry->subscribers.find(endpoint_id);
        if (current == entry->subscribers.end()) {
          return;
        }
        subscriber = current->second;
        entry->subscribers.erase(current);
        if (entry->subscribers.empty()) {
          channels_.erase(channel);
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(subscriber->mutex);
      subscriber->stopping = true;
      subscriber->queue.clear();
      subscriber->cv.notify_all();
    }
    if (subscriber->worker.joinable()) {
      if (subscriber->worker.get_id() == std::this_thread::get_id()) {
        subscriber->worker.detach();
      } else {
        subscriber->worker.join();
      }
    }
  }

  bool HasWriter(const std::string& channel_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto channel = channels_.find(channel_name);
    return channel != channels_.end() && channel->second->reader != nullptr &&
           channel->second->reader->HasWriter();
  }

 private:
  struct Subscriber {
    Callback callback;
    std::deque<std::shared_ptr<message::RawMessage>> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    size_t queue_capacity = 1;
    bool stopping = false;
  };

  struct ChannelEntry {
    std::shared_ptr<Reader<message::RawMessage>> reader;
    std::unordered_map<uint64_t, std::shared_ptr<Subscriber>> subscribers;
    std::mutex mutex;
  };

  static std::shared_ptr<Reader<message::RawMessage>> CreateReader(
      Node* node, const ReaderConfig& config, uint64_t endpoint_id,
      const std::shared_ptr<ChannelEntry>& entry) {
    proto::RoleAttributes attr;
    attr.set_host_name(common::GlobalData::Instance()->HostName());
    attr.set_host_ip(common::GlobalData::Instance()->HostIp());
    attr.set_process_id(common::GlobalData::Instance()->ProcessId());
    const std::string endpoint_name =
        node->Name() + "_gpu_control_" + std::to_string(endpoint_id);
    attr.set_node_name(endpoint_name);
    attr.set_node_id(common::Hash(endpoint_name));
    attr.set_channel_name(config.channel_name);
    attr.set_channel_id(
        common::GlobalData::RegisterChannel(config.channel_name));
    attr.set_message_type(message::MessageType<message::RawMessage>());
    std::string proto_desc;
    message::GetDescriptorString<message::RawMessage>(attr.message_type(),
                                                      &proto_desc);
    attr.set_proto_desc(std::move(proto_desc));
    attr.mutable_qos_profile()->CopyFrom(config.qos_profile);

    std::weak_ptr<ChannelEntry> weak_entry = entry;
    auto reader = std::make_shared<Reader<message::RawMessage>>(
        attr,
        [weak_entry](const std::shared_ptr<message::RawMessage>& message) {
          const auto current = weak_entry.lock();
          if (!current) {
            return;
          }
          std::vector<std::shared_ptr<Subscriber>> subscribers;
          {
            std::lock_guard<std::mutex> lock(current->mutex);
            subscribers.reserve(current->subscribers.size());
            for (const auto& item : current->subscribers) {
              subscribers.push_back(item.second);
            }
          }
          for (const auto& subscriber : subscribers) {
            std::lock_guard<std::mutex> lock(subscriber->mutex);
            if (!subscriber->stopping &&
                subscriber->queue.size() < subscriber->queue_capacity) {
              subscriber->queue.push_back(message);
              subscriber->cv.notify_one();
            }
          }
        },
        std::max(1U, config.pending_queue_size));
    return reader->Init() ? reader : nullptr;
  }

  static void RunSubscriber(const std::shared_ptr<Subscriber>& subscriber) {
    while (true) {
      std::shared_ptr<message::RawMessage> message;
      {
        std::unique_lock<std::mutex> lock(subscriber->mutex);
        subscriber->cv.wait(lock, [&subscriber]() {
          return subscriber->stopping || !subscriber->queue.empty();
        });
        if (subscriber->stopping) {
          return;
        }
        message = std::move(subscriber->queue.front());
        subscriber->queue.pop_front();
      }
      subscriber->callback(message);
    }
  }

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<ChannelEntry>> channels_;
};

inline GpuControlSubscription::~GpuControlSubscription() {
  GpuControlDispatcher::Instance()->Unsubscribe(channel_name_, endpoint_id_);
}

inline bool GpuControlSubscription::HasWriter() const {
  return GpuControlDispatcher::Instance()->HasWriter(channel_name_);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_GPU_CONTROL_ENDPOINT_H_
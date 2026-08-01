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

#ifndef CYBER_SERVICE_SERVICE_H_
#define CYBER_SERVICE_SERVICE_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cyber/common/types.h"
#include "cyber/message/message_traits.h"
#include "cyber/node/node_channel_impl.h"
#include "cyber/proto/rpc.pb.h"
#include "cyber/scheduler/scheduler.h"
#include "cyber/service/rpc_context.h"
#include "cyber/service/rpc_options.h"
#include "cyber/service/rpc_status.h"
#include "cyber/service/service_base.h"

namespace apollo {
namespace cyber {

/**
 * @class Service
 * @brief Service handles `Request` from the Client, and send a `Response` to
 * it.
 *
 * @tparam Request the request type
 * @tparam Response the response type
 */
template <typename Request, typename Response>
class Service : public ServiceBase {
 public:
  using ServiceCallback = std::function<void(const std::shared_ptr<Request>&,
                                             std::shared_ptr<Response>&)>;
  using ContextServiceCallback = std::function<RpcStatus(
      RpcContext&, const std::shared_ptr<Request>&,
      std::shared_ptr<Response>&)>;
  /**
   * @brief Construct a new Service object
   *
   * @param node_name used to fill RoleAttribute when join the topology
   * @param service_name the service name we provide
   * @param service_callback reference of `ServiceCallback` object
   */
  Service(const std::string& node_name, const std::string& service_name,
          const ServiceCallback& service_callback)
      : Service(node_name, service_name, ServiceOptions(), service_callback) {}

  Service(const std::string& node_name, const std::string& service_name,
          const ServiceOptions& options,
          const ServiceCallback& service_callback)
      : ServiceBase(service_name),
        node_name_(node_name),
        options_(options),
        service_callback_(
            [service_callback](RpcContext&,
                               const std::shared_ptr<Request>& request,
                               std::shared_ptr<Response>& response) {
              service_callback(request, response);
              return RpcStatus::OK();
            }),
        request_channel_(options.instance_id == 0
                             ? service_name + SRV_CHANNEL_REQ_SUFFIX
                             : service_name + SRV_CHANNEL_REQ_SUFFIX + "/" +
                                   std::to_string(options.instance_id)),
        response_channel_(service_name + SRV_CHANNEL_RES_SUFFIX) {}

  /**
   * @brief Construct a new Service object
   *
   * @param node_name used to fill RoleAttribute when join the topology
   * @param service_name the service name we provide
   * @param service_callback rvalue reference of `ServiceCallback` object
   */
  Service(const std::string& node_name, const std::string& service_name,
          ServiceCallback&& service_callback)
      : Service(node_name, service_name, ServiceOptions(),
                std::move(service_callback)) {}

  Service(const std::string& node_name, const std::string& service_name,
          const ServiceOptions& options, ServiceCallback&& service_callback)
      : ServiceBase(service_name),
        node_name_(node_name),
        options_(options),
        service_callback_(
            [service_callback = std::move(service_callback)](
                RpcContext&, const std::shared_ptr<Request>& request,
                std::shared_ptr<Response>& response) {
              service_callback(request, response);
              return RpcStatus::OK();
            }),
        request_channel_(options.instance_id == 0
                             ? service_name + SRV_CHANNEL_REQ_SUFFIX
                             : service_name + SRV_CHANNEL_REQ_SUFFIX + "/" +
                                   std::to_string(options.instance_id)),
        response_channel_(service_name + SRV_CHANNEL_RES_SUFFIX) {}

  Service(const std::string& node_name, const std::string& service_name,
          const ServiceOptions& options,
          ContextServiceCallback service_callback)
      : ServiceBase(service_name),
        node_name_(node_name),
        options_(options),
        service_callback_(std::move(service_callback)),
        request_channel_(options.instance_id == 0
                             ? service_name + SRV_CHANNEL_REQ_SUFFIX
                             : service_name + SRV_CHANNEL_REQ_SUFFIX + "/" +
                                   std::to_string(options.instance_id)),
        response_channel_(service_name + SRV_CHANNEL_RES_SUFFIX) {}

  /**
   * @brief Forbid default constructing
   */
  Service() = delete;

  ~Service() { destroy(); }

  /**
   * @brief Init the Service
   */
  bool Init();

  /**
   * @brief Destroy the Service
   */
  void destroy();

  const std::string& request_channel() const { return request_channel_; }

 private:
  void HandleRequest(const std::shared_ptr<proto::RpcRequest>& request,
                     const transport::MessageInfo& message_info,
                     RpcContext::Clock::time_point deadline,
                     const std::string& call_key,
                     const std::shared_ptr<std::atomic<bool>>& cancelled);

  void SendResponse(const transport::MessageInfo& message_info,
                    const std::shared_ptr<proto::RpcResponse>& response);

  bool IsInit(void) const { return request_receiver_ != nullptr; }
  std::string CallKey(const transport::MessageInfo& message_info) const;
  void FinishCall(const std::string& call_key,
                  const std::string& idempotency_key,
                  const std::shared_ptr<proto::RpcResponse>& response);

  std::string node_name_;
  ServiceOptions options_;
  ContextServiceCallback service_callback_;

  std::shared_ptr<transport::Transmitter<proto::RpcResponse>>
      response_transmitter_;
  std::shared_ptr<transport::Receiver<proto::RpcRequest>> request_receiver_;
  std::string request_channel_;
  std::string response_channel_;

  std::atomic<bool> inited_{false};
  bool Enqueue(std::function<void()>&& task);
  void Process();
  std::vector<std::thread> threads_;
  std::mutex queue_mutex_;
  std::condition_variable condition_;
  std::list<std::function<void()>> tasks_;

  std::mutex rpc_state_mutex_;
  std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>>
      active_calls_;
  std::unordered_set<std::string> active_idempotency_keys_;
  std::unordered_map<std::string, std::shared_ptr<proto::RpcResponse>>
      idempotency_cache_;
  std::deque<std::string> idempotency_order_;
};

template <typename Request, typename Response>
void Service<Request, Response>::destroy() {
  if (!inited_.exchange(false)) {
    ShutdownDiscovery();
    return;
  }
  request_receiver_.reset();
  {
    std::lock_guard<std::mutex> lg(queue_mutex_);
    this->tasks_.clear();
  }
  condition_.notify_all();
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
  response_transmitter_.reset();
  ShutdownDiscovery();
}

template <typename Request, typename Response>
inline bool Service<Request, Response>::Enqueue(
    std::function<void()>&& task) {
  std::lock_guard<std::mutex> lg(queue_mutex_);
  if (!inited_.load() ||
      tasks_.size() >= options_.max_pending_requests) {
    return false;
  }
  tasks_.emplace_back(std::move(task));
  condition_.notify_one();
  return true;
}

template <typename Request, typename Response>
void Service<Request, Response>::Process() {
  while (!cyber::IsShutdown()) {
    std::unique_lock<std::mutex> ul(queue_mutex_);
    condition_.wait(
        ul, [this]() { return !inited_.load() || !this->tasks_.empty(); });
    if (!inited_.load()) {
      break;
    }
    if (!tasks_.empty()) {
      auto task = tasks_.front();
      tasks_.pop_front();
      ul.unlock();
      task();
    }
  }
}

template <typename Request, typename Response>
bool Service<Request, Response>::Init() {
  if (IsInit()) {
    return true;
  }
  if (options_.concurrency == 0 || options_.max_pending_requests == 0) {
    AERROR << "Invalid service options for " << service_name_;
    return false;
  }
  proto::RoleAttributes role;
  role.set_node_name(node_name_);
  role.set_channel_name(response_channel_);
  auto channel_id = common::GlobalData::RegisterChannel(response_channel_);
  role.set_channel_id(channel_id);
  role.mutable_qos_profile()->CopyFrom(
      transport::QosProfileConf::QOS_PROFILE_SERVICES_DEFAULT);
  auto transport = transport::Transport::Instance();
  response_transmitter_ =
      transport->CreateTransmitter<proto::RpcResponse>(
          role, proto::OptionalMode::RTPS);
  if (response_transmitter_ == nullptr) {
    AERROR << " Create response pub failed.";
    return false;
  }

  role.set_channel_name(request_channel_);
  channel_id = common::GlobalData::RegisterChannel(request_channel_);
  role.set_channel_id(channel_id);
  request_receiver_ = transport->CreateReceiver<proto::RpcRequest>(
      role,
      [=](const std::shared_ptr<proto::RpcRequest>& request,
          const transport::MessageInfo& message_info,
          const proto::RoleAttributes& reader_attr) {
        (void)reader_attr;
        const auto call_key = CallKey(message_info);
        if (request->cancel()) {
          std::lock_guard<std::mutex> lock(rpc_state_mutex_);
          auto iter = active_calls_.find(call_key);
          if (iter != active_calls_.end()) {
            iter->second->store(true, std::memory_order_release);
          }
          return;
        }
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        {
          std::lock_guard<std::mutex> lock(rpc_state_mutex_);
          active_calls_[call_key] = cancelled;
        }
        const auto deadline =
            RpcContext::Clock::now() +
            std::chrono::nanoseconds(request->timeout_ns());
        auto task =
            [this, request, message_info, deadline, call_key, cancelled]() {
              this->HandleRequest(request, message_info, deadline, call_key,
                                  cancelled);
        };
        if (!Enqueue(std::move(task))) {
          AWARN << "Service request queue is full: " << service_name_;
          auto response = std::make_shared<proto::RpcResponse>();
          response->set_call_id(request->call_id());
          response->set_status(proto::RPC_RESOURCE_EXHAUSTED);
          response->set_error_message("service request queue is full");
          transport::MessageInfo response_info(message_info);
          response_info.set_sender_id(response_transmitter_->id());
          SendResponse(response_info, response);
          std::lock_guard<std::mutex> lock(rpc_state_mutex_);
          active_calls_.erase(call_key);
        }
      },
      proto::OptionalMode::RTPS);
  if (request_receiver_ == nullptr) {
    AERROR << " Create request sub failed." << request_channel_;
    response_transmitter_.reset();
    return false;
  }
  inited_.store(true);
  threads_.reserve(options_.concurrency);
  for (std::uint32_t i = 0; i < options_.concurrency; ++i) {
    threads_.emplace_back(&Service<Request, Response>::Process, this);
  }
  return true;
}

template <typename Request, typename Response>
void Service<Request, Response>::HandleRequest(
    const std::shared_ptr<proto::RpcRequest>& wire_request,
    const transport::MessageInfo& message_info,
    RpcContext::Clock::time_point deadline, const std::string& call_key,
    const std::shared_ptr<std::atomic<bool>>& cancelled) {
  if (!IsInit()) {
    // LOG_DEBUG << "not inited error.";
    return;
  }
  ADEBUG << "handling request:" << request_channel_;
  auto wire_response = std::make_shared<proto::RpcResponse>();
  wire_response->set_call_id(wire_request->call_id());
  const auto& idempotency_key = wire_request->idempotency_key();

  if (!idempotency_key.empty()) {
    std::lock_guard<std::mutex> lock(rpc_state_mutex_);
    auto cached = idempotency_cache_.find(idempotency_key);
    if (cached != idempotency_cache_.end()) {
      wire_response->CopyFrom(*cached->second);
      wire_response->set_call_id(wire_request->call_id());
    } else if (active_idempotency_keys_.count(idempotency_key) != 0) {
      wire_response->set_status(proto::RPC_ALREADY_EXISTS);
      wire_response->set_error_message(
          "request with idempotency key is already running");
    } else {
      active_idempotency_keys_.insert(idempotency_key);
    }
    if (wire_response->has_status()) {
      active_calls_.erase(call_key);
      transport::MessageInfo msg_info(message_info);
      msg_info.set_sender_id(response_transmitter_->id());
      SendResponse(msg_info, wire_response);
      return;
    }
  }

  auto request = std::make_shared<Request>();
  if (!wire_request->has_payload() ||
      !message::ParseFromString(wire_request->payload(), request.get())) {
    wire_response->set_status(proto::RPC_INVALID_ARGUMENT);
    wire_response->set_error_message("failed to parse request payload");
  } else {
    RpcContext context(deadline, cancelled);
    if (context.IsCancelled()) {
      wire_response->set_status(proto::RPC_DEADLINE_EXCEEDED);
      wire_response->set_error_message("request deadline exceeded in queue");
    } else {
      auto response = std::make_shared<Response>();
      RpcStatus status = service_callback_(context, request, response);
      if (status.ok() && context.IsCancellationRequested()) {
        status = RpcStatus(RpcStatusCode::CANCELLED, "request cancelled");
      } else if (status.ok() && context.IsDeadlineExceeded()) {
        status = RpcStatus(RpcStatusCode::DEADLINE_EXCEEDED,
                           "request deadline exceeded");
      }
      wire_response->set_status(
          static_cast<proto::RpcWireStatus>(status.code()));
      wire_response->set_error_message(status.message());
      if (status.ok() &&
          !message::SerializeToString(*response,
                                      wire_response->mutable_payload())) {
        wire_response->set_status(proto::RPC_INTERNAL);
        wire_response->set_error_message("failed to serialize response");
        wire_response->clear_payload();
      }
    }
  }
  transport::MessageInfo msg_info(message_info);
  msg_info.set_sender_id(response_transmitter_->id());
  SendResponse(msg_info, wire_response);
  FinishCall(call_key, idempotency_key, wire_response);
}

template <typename Request, typename Response>
std::string Service<Request, Response>::CallKey(
    const transport::MessageInfo& message_info) const {
  std::string key(message_info.sender_id().data(), transport::ID_SIZE);
  const auto sequence_number = message_info.seq_num();
  key.append(reinterpret_cast<const char*>(&sequence_number),
             sizeof(sequence_number));
  return key;
}

template <typename Request, typename Response>
void Service<Request, Response>::FinishCall(
    const std::string& call_key, const std::string& idempotency_key,
    const std::shared_ptr<proto::RpcResponse>& response) {
  std::lock_guard<std::mutex> lock(rpc_state_mutex_);
  active_calls_.erase(call_key);
  if (idempotency_key.empty()) {
    return;
  }
  active_idempotency_keys_.erase(idempotency_key);
  if (options_.max_idempotency_entries == 0) {
    return;
  }
  while (idempotency_order_.size() >= options_.max_idempotency_entries) {
    idempotency_cache_.erase(idempotency_order_.front());
    idempotency_order_.pop_front();
  }
  auto cached_response = std::make_shared<proto::RpcResponse>();
  cached_response->CopyFrom(*response);
  idempotency_cache_[idempotency_key] = std::move(cached_response);
  idempotency_order_.push_back(idempotency_key);
}

template <typename Request, typename Response>
void Service<Request, Response>::SendResponse(
    const transport::MessageInfo& message_info,
    const std::shared_ptr<proto::RpcResponse>& response) {
  if (!IsInit()) {
    // LOG_DEBUG << "not inited error.";
    return;
  }
  // publish return value ?
  // LOG_DEBUG << "send response id:" << message_id.sequence_number;
  response_transmitter_->Transmit(response, message_info);
}

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SERVICE_SERVICE_H_

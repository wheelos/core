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

#ifndef CYBER_SERVICE_CLIENT_H_
#define CYBER_SERVICE_CLIENT_H_

#include <algorithm>
#include <future>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/common/log.h"
#include "cyber/common/types.h"
#include "cyber/common/util.h"
#include "cyber/message/message_traits.h"
#include "cyber/node/node_channel_impl.h"
#include "cyber/proto/rpc.pb.h"
#include "cyber/service/client_base.h"
#include "cyber/service/rpc_options.h"
#include "cyber/service/rpc_status.h"

namespace apollo {
namespace cyber {

/**
 * @class Client
 * @brief Client get `Response` from a responding `Service` by sending a Request
 *
 * @tparam Request the `Service` request type
 * @tparam Response the `Service` response type
 *
 * @warning One Client can only request one Service
 */
template <typename Request, typename Response>
class Client : public ClientBase {
 public:
  using SharedRequest = typename std::shared_ptr<Request>;
  using SharedResponse = typename std::shared_ptr<Response>;
  using Promise = std::promise<SharedResponse>;
  using SharedPromise = std::shared_ptr<Promise>;
  using SharedFuture = std::shared_future<SharedResponse>;
  using CallbackType = std::function<void(SharedFuture)>;
  using ResultPromise = std::promise<RpcResult<Response>>;
  using SharedResultPromise = std::shared_ptr<ResultPromise>;
  using ResultFuture = std::shared_future<RpcResult<Response>>;

  /**
   * @brief Construct a new Client object
   *
   * @param node_name used to fill RoleAttribute
   * @param service_name service name the Client can request
   */
  Client(const std::string& node_name, const std::string& service_name)
      : ClientBase(service_name),
        node_name_(node_name),
        request_channel_(service_name + SRV_CHANNEL_REQ_SUFFIX),
        response_channel_(service_name + SRV_CHANNEL_RES_SUFFIX),
        sequence_number_(0) {}

  /**
   * @brief forbid Constructing a new Client object with empty params
   */
  Client() = delete;

  virtual ~Client() { Destroy(); }

  /**
   * @brief Init the Client
   *
   * @return true if init successfully
   * @return false if init failed
   */
  bool Init();

  /**
   * @brief Request the Service with a shared ptr Request type
   *
   * @param request shared ptr of Request type
   * @param timeout_s request timeout, if timeout, response will be empty
   * @return SharedResponse result of this request
   */
  SharedResponse SendRequest(
      SharedRequest request,
      const std::chrono::seconds& timeout_s = std::chrono::seconds(5));

  /**
   * @brief Request the Service with a Request object
   *
   * @param request Request object
   * @param timeout_s request timeout, if timeout, response will be empty
   * @return SharedResponse result of this request
   */
  SharedResponse SendRequest(
      const Request& request,
      const std::chrono::seconds& timeout_s = std::chrono::seconds(5));

  RpcResult<Response> Call(SharedRequest request,
                           const RpcCallOptions& options = RpcCallOptions());

  RpcResult<Response> Call(const Request& request,
                           const RpcCallOptions& options = RpcCallOptions());

  ResultFuture AsyncCall(SharedRequest request, const RpcCallOptions& options,
                         uint64_t* call_id = nullptr);

  ResultFuture AsyncCall(const Request& request,
                         const RpcCallOptions& options,
                         uint64_t* call_id = nullptr);

  bool CancelRequest(uint64_t call_id);

  /**
   * @brief Send Request shared ptr asynchronously
   */
  SharedFuture AsyncSendRequest(SharedRequest request);

  /**
   * @brief Send Request object asynchronously
   */
  SharedFuture AsyncSendRequest(const Request& request);

  /**
   * @brief Send Request shared ptr asynchronously and invoke `cb` after we get
   * response
   *
   * @param request Request shared ptr
   * @param cb callback function after we get response
   * @return SharedFuture a `std::future` shared ptr
   */
  SharedFuture AsyncSendRequest(SharedRequest request, CallbackType&& cb);

  /**
   * @brief Is the Service is ready?
   */
  bool ServiceIsReady() const;

  /**
   * @brief destroy this Client
   */
  void Destroy();

  /**
   * @brief wait for the connection with the Service established
   *
   * @tparam RatioT timeout unit, default is std::milli
   * @param timeout wait time in unit of `RatioT`
   * @return true if the connection established
   * @return false if timeout
   */
  template <typename RatioT = std::milli>
  bool WaitForService(std::chrono::duration<int64_t, RatioT> timeout =
                          std::chrono::duration<int64_t, RatioT>(-1)) {
    return WaitForServiceNanoseconds(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout),
        [this]() { return ServiceIsReady(); });
  }

 private:
  struct PendingRequest {
    SharedPromise promise;
    CallbackType callback;
    SharedFuture future;
    SharedResultPromise result_promise;
    ResultFuture result_future;
  };

  SharedFuture AsyncSendRequestInternal(SharedRequest request,
                                        CallbackType&& cb,
                                        const RpcCallOptions& options,
                                        uint64_t* sequence_number,
                                        ResultFuture* result_future);
  bool CompleteRequest(uint64_t sequence_number,
                       const RpcResult<Response>& result);
  bool SendCancellation(uint64_t call_id);
  void GetCompatibleServers(
      std::vector<proto::RoleAttributes>* servers) const;
  bool RefreshRequestTransmitter();

  void HandleResponse(const std::shared_ptr<proto::RpcResponse>& response,
                      const transport::MessageInfo& request_info);

  bool IsInit(void) const { return response_receiver_ != nullptr; }

  std::string node_name_;

  std::function<void(const std::shared_ptr<proto::RpcResponse>&,
                     const transport::MessageInfo&)>
      response_callback_;

  std::unordered_map<uint64_t, PendingRequest> pending_requests_;
  std::mutex pending_requests_mutex_;

  std::shared_ptr<transport::Transmitter<proto::RpcRequest>>
      request_transmitter_;
  std::shared_ptr<transport::Receiver<proto::RpcResponse>> response_receiver_;
  std::mutex endpoint_mutex_;
  std::string request_channel_;
  std::string response_channel_;

  transport::Identity writer_id_;
  uint64_t sequence_number_;
};

template <typename Request, typename Response>
void Client<Request, Response>::Destroy() {
  response_receiver_.reset();
  request_transmitter_.reset();

  std::unordered_map<uint64_t, PendingRequest> pending_requests;
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    pending_requests.swap(pending_requests_);
  }
  for (auto& item : pending_requests) {
    RpcResult<Response> result(
        RpcStatus(RpcStatusCode::CANCELLED, "client destroyed"));
    item.second.promise->set_value(nullptr);
    item.second.result_promise->set_value(result);
  }
  ShutdownDiscovery();
}

template <typename Request, typename Response>
bool Client<Request, Response>::Init() {
  if (IsInit()) {
    return true;
  }
  proto::RoleAttributes role;
  role.set_node_name(node_name_);
  role.mutable_qos_profile()->CopyFrom(
      transport::QosProfileConf::QOS_PROFILE_SERVICES_DEFAULT);
  auto transport = transport::Transport::Instance();

  response_callback_ =
      std::bind(&Client<Request, Response>::HandleResponse, this,
                std::placeholders::_1, std::placeholders::_2);

  role.set_channel_name(response_channel_);
  auto channel_id = common::GlobalData::RegisterChannel(response_channel_);
  role.set_channel_id(channel_id);
  response_receiver_ = transport->CreateReceiver<proto::RpcResponse>(
      role,
      [=](const std::shared_ptr<proto::RpcResponse>& response,
          const transport::MessageInfo& message_info,
          const proto::RoleAttributes& reader_attr) {
        (void)message_info;
        (void)reader_attr;
        response_callback_(response, message_info);
      },
      proto::OptionalMode::RTPS);
  if (response_receiver_ == nullptr) {
    AERROR << "Create response sub failed.";
    return false;
  }
  RefreshRequestTransmitter();
  return true;
}

template <typename Request, typename Response>
typename Client<Request, Response>::SharedResponse
Client<Request, Response>::SendRequest(SharedRequest request,
                                       const std::chrono::seconds& timeout_s) {
  if (!IsInit()) {
    return nullptr;
  }
  uint64_t sequence_number = 0;
  RpcCallOptions options;
  options.timeout = timeout_s;
  auto future = AsyncSendRequestInternal(
      request, [](SharedFuture) {}, options, &sequence_number, nullptr);
  if (!future.valid()) {
    return nullptr;
  }
  auto status = future.wait_for(timeout_s);
  if (status == std::future_status::ready) {
    return future.get();
  } else {
    SendCancellation(sequence_number);
    CompleteRequest(
        sequence_number,
        RpcResult<Response>(RpcStatus(RpcStatusCode::DEADLINE_EXCEEDED,
                                      "request deadline exceeded")));
    return nullptr;
  }
}

template <typename Request, typename Response>
typename Client<Request, Response>::SharedResponse
Client<Request, Response>::SendRequest(const Request& request,
                                       const std::chrono::seconds& timeout_s) {
  if (!IsInit()) {
    return nullptr;
  }
  auto request_ptr = std::make_shared<Request>(request);
  return SendRequest(request_ptr, timeout_s);
}

template <typename Request, typename Response>
RpcResult<Response> Client<Request, Response>::Call(
    SharedRequest request, const RpcCallOptions& options) {
  if (request == nullptr) {
    return RpcResult<Response>(
        RpcStatus(RpcStatusCode::INVALID_ARGUMENT, "request is null"));
  }
  if (options.timeout.count() < 0) {
    return RpcResult<Response>(
        RpcStatus(RpcStatusCode::INVALID_ARGUMENT, "timeout is negative"));
  }
  if (!IsInit() || !ServiceIsReady()) {
    return RpcResult<Response>(
        RpcStatus(RpcStatusCode::UNAVAILABLE, "service is not ready"));
  }

  uint64_t sequence_number = 0;
  ResultFuture result_future;
  auto future = AsyncSendRequestInternal(request, [](SharedFuture) {}, options,
                                         &sequence_number, &result_future);
  if (!future.valid() || !result_future.valid()) {
    return RpcResult<Response>(
        RpcStatus(RpcStatusCode::UNAVAILABLE, "request was not sent"));
  }
  if (result_future.wait_for(options.timeout) != std::future_status::ready) {
    SendCancellation(sequence_number);
    CompleteRequest(
        sequence_number,
        RpcResult<Response>(RpcStatus(RpcStatusCode::DEADLINE_EXCEEDED,
                                      "request deadline exceeded")));
    return RpcResult<Response>(RpcStatus(RpcStatusCode::DEADLINE_EXCEEDED,
                                         "request deadline exceeded"));
  }
  return result_future.get();
}

template <typename Request, typename Response>
typename Client<Request, Response>::ResultFuture
Client<Request, Response>::AsyncCall(SharedRequest request,
                                     const RpcCallOptions& options,
                                     uint64_t* call_id) {
  ResultFuture result_future;
  auto future = AsyncSendRequestInternal(request, [](SharedFuture) {}, options,
                                         call_id, &result_future);
  if (!future.valid()) {
    return ResultFuture();
  }
  return result_future;
}

template <typename Request, typename Response>
typename Client<Request, Response>::ResultFuture
Client<Request, Response>::AsyncCall(const Request& request,
                                     const RpcCallOptions& options,
                                     uint64_t* call_id) {
  return AsyncCall(std::make_shared<Request>(request), options, call_id);
}

template <typename Request, typename Response>
bool Client<Request, Response>::CancelRequest(uint64_t call_id) {
  if (!SendCancellation(call_id)) {
    return false;
  }
  return CompleteRequest(
      call_id,
      RpcResult<Response>(
          RpcStatus(RpcStatusCode::CANCELLED, "request cancelled")));
}

template <typename Request, typename Response>
RpcResult<Response> Client<Request, Response>::Call(
    const Request& request, const RpcCallOptions& options) {
  return Call(std::make_shared<Request>(request), options);
}

template <typename Request, typename Response>
typename Client<Request, Response>::SharedFuture
Client<Request, Response>::AsyncSendRequest(const Request& request) {
  auto request_ptr = std::make_shared<Request>(request);
  return AsyncSendRequest(request_ptr);
}

template <typename Request, typename Response>
typename Client<Request, Response>::SharedFuture
Client<Request, Response>::AsyncSendRequest(SharedRequest request) {
  return AsyncSendRequest(request, [](SharedFuture) {});
}

template <typename Request, typename Response>
typename Client<Request, Response>::SharedFuture
Client<Request, Response>::AsyncSendRequest(SharedRequest request,
                                            CallbackType&& cb) {
  return AsyncSendRequestInternal(request, std::move(cb), RpcCallOptions(),
                                  nullptr, nullptr);
}

template <typename Request, typename Response>
typename Client<Request, Response>::SharedFuture
Client<Request, Response>::AsyncSendRequestInternal(
    SharedRequest request, CallbackType&& cb, const RpcCallOptions& options,
    uint64_t* sequence_number, ResultFuture* result_future) {
  if (!IsInit() || request == nullptr || !RefreshRequestTransmitter()) {
    return SharedFuture();
  }
  if (options.timeout.count() < 0) {
    return SharedFuture();
  }

  auto wire_request = std::make_shared<proto::RpcRequest>();
  if (!message::SerializeToString(*request, wire_request->mutable_payload())) {
    return SharedFuture();
  }
  wire_request->set_timeout_ns(
      static_cast<std::uint64_t>(options.timeout.count()));
  wire_request->set_priority(static_cast<std::uint32_t>(options.priority));
  wire_request->set_trace_id(options.trace_id);
  wire_request->set_idempotency_key(options.idempotency_key);

  SharedPromise call_promise = std::make_shared<Promise>();
  SharedFuture future(call_promise->get_future());
  SharedResultPromise result_promise = std::make_shared<ResultPromise>();
  ResultFuture local_result_future(result_promise->get_future());
  uint64_t current_sequence = 0;
  std::shared_ptr<transport::Transmitter<proto::RpcRequest>>
      request_transmitter;
  transport::Identity writer_id;
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    request_transmitter = request_transmitter_;
    writer_id = writer_id_;
  }
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    current_sequence = ++sequence_number_;
    pending_requests_.emplace(current_sequence,
                              PendingRequest{call_promise, std::move(cb),
                                             future, result_promise,
                                             local_result_future});
  }
  wire_request->set_call_id(current_sequence);
  if (sequence_number != nullptr) {
    *sequence_number = current_sequence;
  }
  if (result_future != nullptr) {
    *result_future = local_result_future;
  }

  transport::MessageInfo info(writer_id, current_sequence, writer_id);
  if (!request_transmitter->Transmit(wire_request, info)) {
    CompleteRequest(
        current_sequence,
        RpcResult<Response>(
            RpcStatus(RpcStatusCode::UNAVAILABLE, "request was not sent")));
  }
  return future;
}

template <typename Request, typename Response>
bool Client<Request, Response>::ServiceIsReady() const {
  std::vector<proto::RoleAttributes> servers;
  GetCompatibleServers(&servers);
  return !servers.empty();
}

template <typename Request, typename Response>
void Client<Request, Response>::GetCompatibleServers(
    std::vector<proto::RoleAttributes>* compatible_servers) const {
  if (compatible_servers == nullptr) {
    return;
  }
  std::vector<proto::RoleAttributes> servers;
  service_discovery::TopologyManager::Instance()
      ->service_manager()
      ->GetServers(service_name_, &servers);
  for (auto& server : servers) {
    if (server.has_service_request_type() &&
        server.service_request_type() == message::MessageType<Request>() &&
        server.has_service_response_type() &&
        server.service_response_type() == message::MessageType<Response>() &&
        server.has_service_interface_major() &&
        server.service_interface_major() == 1 &&
        server.has_service_request_channel()) {
      compatible_servers->emplace_back(std::move(server));
    }
  }
  std::sort(compatible_servers->begin(), compatible_servers->end(),
            [](const proto::RoleAttributes& lhs,
               const proto::RoleAttributes& rhs) {
              return lhs.service_instance_id() < rhs.service_instance_id();
            });
}

template <typename Request, typename Response>
bool Client<Request, Response>::RefreshRequestTransmitter() {
  std::vector<proto::RoleAttributes> servers;
  GetCompatibleServers(&servers);
  if (servers.empty()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(endpoint_mutex_);
  if (request_transmitter_ != nullptr) {
    for (const auto& server : servers) {
      if (request_channel_ == server.service_request_channel()) {
        return true;
      }
    }
  }
  {
    std::lock_guard<std::mutex> pending_lock(pending_requests_mutex_);
    if (!pending_requests_.empty()) {
      return false;
    }
  }

  proto::RoleAttributes role;
  role.set_node_name(node_name_);
  const auto server_index =
      common::Hash(node_name_ + service_name_) % servers.size();
  const auto& server = servers[server_index];
  request_channel_ = server.service_request_channel();
  role.set_channel_name(request_channel_);
  role.set_channel_id(common::GlobalData::RegisterChannel(request_channel_));
  role.mutable_qos_profile()->CopyFrom(
      transport::QosProfileConf::QOS_PROFILE_SERVICES_DEFAULT);
  auto transmitter = transport::Transport::Instance()
                         ->CreateTransmitter<proto::RpcRequest>(
                             role, proto::OptionalMode::RTPS);
  if (transmitter == nullptr) {
    AERROR << "Create request pub failed: " << request_channel_;
    return false;
  }
  request_transmitter_ = std::move(transmitter);
  writer_id_ = request_transmitter_->id();
  return true;
}

template <typename Request, typename Response>
bool Client<Request, Response>::CompleteRequest(
    uint64_t sequence_number, const RpcResult<Response>& result) {
  PendingRequest pending_request;
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    auto iter = pending_requests_.find(sequence_number);
    if (iter == pending_requests_.end()) {
      return false;
    }
    pending_request = std::move(iter->second);
    pending_requests_.erase(iter);
  }

  pending_request.promise->set_value(result.response());
  pending_request.result_promise->set_value(result);
  if (pending_request.callback) {
    pending_request.callback(pending_request.future);
  }
  return true;
}

template <typename Request, typename Response>
bool Client<Request, Response>::SendCancellation(uint64_t call_id) {
  std::shared_ptr<transport::Transmitter<proto::RpcRequest>> transmitter;
  transport::Identity writer_id;
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    transmitter = request_transmitter_;
    writer_id = writer_id_;
  }
  if (transmitter == nullptr) {
    return false;
  }
  auto cancellation = std::make_shared<proto::RpcRequest>();
  cancellation->set_call_id(call_id);
  cancellation->set_cancel(true);
  transport::MessageInfo info(writer_id, call_id, writer_id);
  return transmitter->Transmit(cancellation, info);
}

template <typename Request, typename Response>
void Client<Request, Response>::HandleResponse(
    const std::shared_ptr<proto::RpcResponse>& response,
    const transport::MessageInfo& request_header) {
  ADEBUG << "client recv response.";
  transport::Identity writer_id;
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    writer_id = writer_id_;
  }
  if (request_header.spare_id() != writer_id) {
    return;
  }
  if (response == nullptr ||
      (response->has_call_id() &&
       response->call_id() != request_header.seq_num())) {
    return;
  }

  const int status_value = static_cast<int>(response->status());
  if (status_value < static_cast<int>(RpcStatusCode::OK) ||
      status_value > static_cast<int>(RpcStatusCode::INTERNAL)) {
    CompleteRequest(
        request_header.seq_num(),
        RpcResult<Response>(
            RpcStatus(RpcStatusCode::INTERNAL, "invalid response status")));
    return;
  }
  const auto status_code = static_cast<RpcStatusCode>(status_value);
  if (status_code != RpcStatusCode::OK) {
    CompleteRequest(
        request_header.seq_num(),
        RpcResult<Response>(RpcStatus(status_code, response->error_message())));
    return;
  }

  auto parsed_response = std::make_shared<Response>();
  if (!response->has_payload() ||
      !message::ParseFromString(response->payload(), parsed_response.get())) {
    CompleteRequest(
        request_header.seq_num(),
        RpcResult<Response>(
            RpcStatus(RpcStatusCode::INTERNAL,
                      "failed to parse response payload")));
    return;
  }
  CompleteRequest(request_header.seq_num(),
                  RpcResult<Response>(std::move(parsed_response)));
}

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SERVICE_CLIENT_H_

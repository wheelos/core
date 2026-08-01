/******************************************************************************
 * Copyright 2026 WheelOS Authors. All Rights Reserved.
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

#ifndef CYBER_SERVICE_RPC_STATUS_H_
#define CYBER_SERVICE_RPC_STATUS_H_

#include <memory>
#include <string>
#include <utility>

namespace apollo {
namespace cyber {

enum class RpcStatusCode {
  OK = 0,
  INVALID_ARGUMENT,
  NOT_FOUND,
  ALREADY_EXISTS,
  UNAVAILABLE,
  DEADLINE_EXCEEDED,
  CANCELLED,
  RESOURCE_EXHAUSTED,
  FAILED_PRECONDITION,
  PERMISSION_DENIED,
  INCOMPATIBLE_TYPE,
  INTERNAL,
};

class RpcStatus {
 public:
  RpcStatus() = default;
  RpcStatus(RpcStatusCode code, std::string message = "")
      : code_(code), message_(std::move(message)) {}

  static RpcStatus OK() { return RpcStatus(); }

  bool ok() const { return code_ == RpcStatusCode::OK; }
  RpcStatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  RpcStatusCode code_ = RpcStatusCode::OK;
  std::string message_;
};

template <typename Response>
class RpcResult {
 public:
  explicit RpcResult(RpcStatus status) : status_(std::move(status)) {}
  explicit RpcResult(std::shared_ptr<Response> response)
      : status_(response == nullptr
                    ? RpcStatus(RpcStatusCode::INTERNAL, "empty response")
                    : RpcStatus::OK()),
        response_(std::move(response)) {}

  bool ok() const { return status_.ok(); }
  const RpcStatus& status() const { return status_; }
  const std::shared_ptr<Response>& response() const { return response_; }

  explicit operator bool() const { return ok(); }

 private:
  RpcStatus status_;
  std::shared_ptr<Response> response_;
};

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SERVICE_RPC_STATUS_H_

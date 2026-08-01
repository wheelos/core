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

#ifndef CYBER_SERVICE_RPC_OPTIONS_H_
#define CYBER_SERVICE_RPC_OPTIONS_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace apollo {
namespace cyber {

enum class RpcPriority : std::uint8_t {
  LOW = 0,
  NORMAL,
  HIGH,
  SAFETY_CRITICAL,
};

enum class ServiceOverflowPolicy : std::uint8_t {
  REJECT_NEW = 0,
};

enum class ServiceInstancePolicy : std::uint8_t {
  EXCLUSIVE = 0,
  REPLICATED,
};

struct RpcCallOptions {
  std::chrono::nanoseconds timeout = std::chrono::seconds(5);
  RpcPriority priority = RpcPriority::NORMAL;
  std::string trace_id;
  std::string idempotency_key;
};

struct ServiceOptions {
  std::size_t max_pending_requests = 1024;
  std::uint32_t concurrency = 1;
  std::uint32_t interface_major = 1;
  std::uint32_t interface_minor = 0;
  std::uint64_t instance_id = 0;
  std::size_t max_idempotency_entries = 1024;
  ServiceInstancePolicy instance_policy = ServiceInstancePolicy::EXCLUSIVE;
  RpcPriority priority = RpcPriority::NORMAL;
  ServiceOverflowPolicy overflow_policy =
      ServiceOverflowPolicy::REJECT_NEW;
};

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SERVICE_RPC_OPTIONS_H_

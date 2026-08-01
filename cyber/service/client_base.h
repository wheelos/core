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

#ifndef CYBER_SERVICE_CLIENT_BASE_H_
#define CYBER_SERVICE_CLIENT_BASE_H_

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "cyber/common/macros.h"
#include "cyber/service_discovery/topology_manager.h"
#include "cyber/state.h"

namespace apollo {
namespace cyber {

/**
 * @class ClientBase
 * @brief Base class of Client
 *
 */
class ClientBase {
 public:
  /**
   * @brief Construct a new Client Base object
   *
   * @param service_name the service we can request
   */
  explicit ClientBase(const std::string& service_name)
      : service_name_(service_name) {}
  virtual ~ClientBase() {}

  /**
   * @brief Destroy the Client
   */
  virtual void Destroy() = 0;

  /**
   * @brief Get the service name
   */
  const std::string& ServiceName() const { return service_name_; }

  /**
   * @brief Ensure whether there is any Service named `service_name_`
   */
  virtual bool ServiceIsReady() const = 0;

  void SetOnShutdown(std::function<void()>&& on_shutdown) {
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    on_shutdown_ = std::move(on_shutdown);
  }

 protected:
  std::string service_name_;

  bool WaitForServiceNanoseconds(
      std::chrono::nanoseconds time_out,
      const std::function<bool()>& is_ready = std::function<bool()>()) {
    constexpr auto kPollInterval = std::chrono::milliseconds(5);
    const bool wait_forever = time_out.count() < 0;
    const auto deadline = std::chrono::steady_clock::now() + time_out;
    while (!cyber::IsShutdown()) {
      const bool ready =
          is_ready
              ? is_ready()
              : service_discovery::TopologyManager::Instance()
                    ->service_manager()
                    ->HasService(service_name_);
      if (ready) {
        return true;
      }
      if (!wait_forever && std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      auto sleep_duration = kPollInterval;
      if (!wait_forever) {
        sleep_duration =
            std::min(kPollInterval,
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - std::chrono::steady_clock::now()));
        if (sleep_duration.count() <= 0) {
          return false;
        }
      }
      std::this_thread::sleep_for(sleep_duration);
    }
    return false;
  }

  void ShutdownDiscovery() {
    std::function<void()> on_shutdown;
    {
      std::lock_guard<std::mutex> lock(shutdown_mutex_);
      on_shutdown = std::move(on_shutdown_);
    }
    if (on_shutdown) {
      on_shutdown();
    }
  }

 private:
  std::mutex shutdown_mutex_;
  std::function<void()> on_shutdown_;
};

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SERVICE_CLIENT_BASE_H_

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

#ifndef CYBER_SERVICE_RPC_CONTEXT_H_
#define CYBER_SERVICE_RPC_CONTEXT_H_

#include <atomic>
#include <chrono>
#include <memory>

namespace apollo {
namespace cyber {

class RpcContext {
 public:
  using Clock = std::chrono::steady_clock;

  RpcContext()
      : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}

  RpcContext(Clock::time_point deadline,
             std::shared_ptr<std::atomic<bool>> cancelled)
      : deadline_(deadline),
        has_deadline_(true),
        cancelled_(std::move(cancelled)) {}

  bool HasDeadline() const { return has_deadline_; }

  std::chrono::nanoseconds RemainingTime() const {
    if (!has_deadline_) {
      return std::chrono::nanoseconds::max();
    }
    const auto now = Clock::now();
    if (now >= deadline_) {
      return std::chrono::nanoseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(deadline_ -
                                                                now);
  }

  bool IsCancelled() const {
    return IsCancellationRequested() || IsDeadlineExceeded();
  }

  bool IsCancellationRequested() const {
    return cancelled_->load(std::memory_order_acquire);
  }

  bool IsDeadlineExceeded() const {
    return has_deadline_ && Clock::now() >= deadline_;
  }

  void Cancel() { cancelled_->store(true, std::memory_order_release); }

 private:
  Clock::time_point deadline_;
  bool has_deadline_ = false;
  std::shared_ptr<std::atomic<bool>> cancelled_;
};

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SERVICE_RPC_CONTEXT_H_

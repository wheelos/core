/******************************************************************************
 * Copyright 2026 WheelOS Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *****************************************************************************/

#ifndef CYBER_ACTION_ACTION_CONTEXT_H_
#define CYBER_ACTION_ACTION_CONTEXT_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace apollo {
namespace cyber {

template <typename Feedback>
class ActionContext {
 public:
  using Clock = std::chrono::steady_clock;
  using FeedbackPublisher = std::function<bool(const Feedback&)>;

  ActionContext(std::string goal_id, Clock::time_point deadline,
                std::shared_ptr<std::atomic<bool>> cancelled,
                FeedbackPublisher feedback_publisher)
      : goal_id_(std::move(goal_id)),
        deadline_(deadline),
        cancelled_(std::move(cancelled)),
        feedback_publisher_(std::move(feedback_publisher)) {}

  const std::string& goal_id() const { return goal_id_; }

  bool IsCancellationRequested() const {
    return cancelled_->load(std::memory_order_acquire);
  }

  bool IsDeadlineExceeded() const { return Clock::now() >= deadline_; }

  bool IsCancelled() const {
    return IsCancellationRequested() || IsDeadlineExceeded();
  }

  std::chrono::nanoseconds RemainingTime() const {
    const auto now = Clock::now();
    if (now >= deadline_) {
      return std::chrono::nanoseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(deadline_ -
                                                                now);
  }

  bool PublishFeedback(const Feedback& feedback) const {
    return !IsCancelled() && feedback_publisher_ &&
           feedback_publisher_(feedback);
  }

 private:
  std::string goal_id_;
  Clock::time_point deadline_;
  std::shared_ptr<std::atomic<bool>> cancelled_;
  FeedbackPublisher feedback_publisher_;
};

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_ACTION_ACTION_CONTEXT_H_

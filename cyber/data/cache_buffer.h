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

#ifndef CYBER_DATA_CACHE_BUFFER_H_
#define CYBER_DATA_CACHE_BUFFER_H_

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "cyber/metrics/metrics.h"

namespace apollo {
namespace cyber {
namespace data {

template <typename T>
class CacheBuffer {
 public:
  using value_type = T;
  using size_type = std::size_t;
  using FusionCallback = std::function<void(const T&)>;

  explicit CacheBuffer(uint64_t size) {
    capacity_ = size + 1;
    buffer_.resize(capacity_);
  }

  CacheBuffer(const CacheBuffer& rhs) {
    std::lock_guard<std::mutex> lg(rhs.mutex_);
    head_ = rhs.head_;
    tail_ = rhs.tail_;
    buffer_ = rhs.buffer_;
    capacity_ = rhs.capacity_;
    fusion_callback_ = rhs.fusion_callback_;
    metric_ = rhs.metric_;
    stamps_ = rhs.stamps_;
    next_unread_ = rhs.next_unread_;
    tracks_shutdown_discard_ = false;
  }

  ~CacheBuffer() {
    if (!metric_ || !tracks_shutdown_discard_) return;
    uint64_t pending = 0;
    if (tail_ > 0) {
      if (next_unread_ == 0) {
        pending = 1;
      } else {
        const auto first_unread = std::max(next_unread_, head_ + 1);
        if (tail_ >= first_unread) {
          pending = tail_ - first_unread + 1;
        }
      }
    }
    metric_->RecordShutdownDiscard(pending);
    metric_->SetQueueDepth(0);
    metrics::Registry::Instance().Retire(metric_);
  }

  void AttachMetrics(const std::shared_ptr<metrics::Endpoint>& endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    metric_ = endpoint;
    if (metric_) {
      tracks_shutdown_discard_ = true;
      stamps_.resize(capacity_);
    }
  }

  void RecordFetch(uint64_t index, uint64_t skipped,
                   metrics::DropReason reason) {
    if (!metric_) {
      return;
    }
    if (skipped) {
      metric_->RecordDrop(reason, skipped);
    }
    metric_->RecordDequeue();
    auto now = metrics::Clock::now();
    metric_->ObserveQueueLatency(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - stamps_[GetIndex(index)]),
        now);
    next_unread_ = index + 1;
    metric_->SetQueueDepth(tail_ >= next_unread_ ? tail_ - next_unread_ + 1 : 0,
                           now);
  }

  T& operator[](const uint64_t& pos) { return buffer_[GetIndex(pos)]; }
  const T& at(const uint64_t& pos) const { return buffer_[GetIndex(pos)]; }

  uint64_t Head() const { return head_ + 1; }
  uint64_t Tail() const { return tail_; }
  uint64_t Size() const { return tail_ - head_; }

  const T& Front() const { return buffer_[GetIndex(head_ + 1)]; }
  const T& Back() const { return buffer_[GetIndex(tail_)]; }

  bool Empty() const { return tail_ == 0; }
  bool Full() const { return capacity_ - 1 == tail_ - head_; }
  uint64_t Capacity() const { return capacity_; }

  void SetFusionCallback(const FusionCallback& callback) {
    fusion_callback_ = callback;
  }

  void Fill(const T& value) {
    if (fusion_callback_) {
      fusion_callback_(value);
    } else {
      if (metric_) {
        auto now = metrics::Clock::now();
        const auto next = tail_ + 1;
        if (next_unread_ == 0 && tail_ > 0) {
          metric_->RecordDrop(metrics::DropReason::InitialSkipToLatest);
        } else if (Full() && next_unread_ != 0 && head_ + 1 >= next_unread_) {
          metric_->RecordDrop(metrics::DropReason::OverflowOverwrite);
        }
        stamps_[GetIndex(next)] = now;
        metric_->RecordEnqueue();
        metric_->SetQueueDepth(
            next_unread_ == 0
                ? 1
                : std::min<uint64_t>(std::max<uint64_t>(1, capacity_ - 1),
                                     next - next_unread_ + 1),
            now);
      }
      if (Full()) {
        buffer_[GetIndex(head_)] = value;
        ++head_;
        ++tail_;
      } else {
        buffer_[GetIndex(tail_ + 1)] = value;
        ++tail_;
      }
    }
  }

  std::mutex& Mutex() { return mutex_; }

 private:
  CacheBuffer& operator=(const CacheBuffer& other) = delete;
  uint64_t GetIndex(const uint64_t& pos) const { return pos % capacity_; }

  uint64_t head_ = 0;
  uint64_t tail_ = 0;
  uint64_t capacity_ = 0;
  std::vector<T> buffer_;
  std::shared_ptr<metrics::Endpoint> metric_;
  std::vector<metrics::TimePoint> stamps_;
  uint64_t next_unread_ = 0;
  bool tracks_shutdown_discard_ = true;
  mutable std::mutex mutex_;
  FusionCallback fusion_callback_;
};

}  // namespace data
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_DATA_CACHE_BUFFER_H_

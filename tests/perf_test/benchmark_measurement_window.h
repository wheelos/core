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

#ifndef TESTS_PERF_TEST_BENCHMARK_MEASUREMENT_WINDOW_H_
#define TESTS_PERF_TEST_BENCHMARK_MEASUREMENT_WINDOW_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

struct MeasurementWindowSnapshot {
  uint64_t received_messages = 0;
  uint64_t received_bytes = 0;
  uint64_t dropped_samples = 0;
  std::vector<uint64_t> latency_samples;
  std::vector<uint64_t> received_per_endpoint;

  bool DeliveryConfirmed() const {
    return !received_per_endpoint.empty() &&
           std::all_of(received_per_endpoint.begin(),
                       received_per_endpoint.end(),
                       [](uint64_t received) { return received > 0; });
  }
};

class MeasurementWindowMetrics {
 public:
  MeasurementWindowMetrics(size_t endpoint_count, size_t latency_sample_cap)
      : latency_sample_cap_(latency_sample_cap),
        received_per_endpoint_(endpoint_count, 0) {
    latency_samples_.reserve(latency_sample_cap_);
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = true;
  }

  bool Record(size_t endpoint_index, uint64_t received_bytes,
              uint64_t latency_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || endpoint_index >= received_per_endpoint_.size()) {
      return false;
    }
    ++received_messages_;
    received_bytes_ += received_bytes;
    ++received_per_endpoint_[endpoint_index];
    if (latency_samples_.size() < latency_sample_cap_) {
      latency_samples_.push_back(latency_ns);
    } else {
      ++dropped_samples_;
    }
    return true;
  }

  MeasurementWindowSnapshot StopAndSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    MeasurementWindowSnapshot snapshot;
    snapshot.received_messages = received_messages_;
    snapshot.received_bytes = received_bytes_;
    snapshot.dropped_samples = dropped_samples_;
    snapshot.latency_samples = latency_samples_;
    snapshot.received_per_endpoint = received_per_endpoint_;
    return snapshot;
  }

 private:
  const size_t latency_sample_cap_;
  std::mutex mutex_;
  bool active_ = false;
  uint64_t received_messages_ = 0;
  uint64_t received_bytes_ = 0;
  uint64_t dropped_samples_ = 0;
  std::vector<uint64_t> latency_samples_;
  std::vector<uint64_t> received_per_endpoint_;
};

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // TESTS_PERF_TEST_BENCHMARK_MEASUREMENT_WINDOW_H_

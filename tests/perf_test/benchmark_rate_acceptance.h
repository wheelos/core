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

#ifndef TESTS_PERF_TEST_BENCHMARK_RATE_ACCEPTANCE_H_
#define TESTS_PERF_TEST_BENCHMARK_RATE_ACCEPTANCE_H_

#include <algorithm>
#include <cstdint>

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

struct AchievedRateAcceptance {
  double target_send_rate_hz = 0.0;
  double target_receive_rate_hz = 0.0;
  double achieved_send_rate_hz = 0.0;
  double achieved_receive_rate_hz = 0.0;
  double achieved_send_ratio = 0.0;
  double achieved_receive_ratio = 0.0;
  double min_achieved_rate_ratio = 0.95;
  bool accepted = true;
};

inline AchievedRateAcceptance EvaluateAchievedRates(
    int frequency_hz, int publishers, int subscribers, int duration_s,
    uint64_t measured_sent_messages, uint64_t measured_received_messages,
    double min_achieved_rate_ratio) {
  AchievedRateAcceptance evaluation;
  evaluation.min_achieved_rate_ratio =
      std::max(0.0, std::min(1.0, min_achieved_rate_ratio));
  if (frequency_hz <= 0) {
    return evaluation;
  }

  const double bounded_publishers = static_cast<double>(std::max(1, publishers));
  const double bounded_subscribers =
      static_cast<double>(std::max(1, subscribers));
  const double measurement_seconds =
      static_cast<double>(std::max(1, duration_s));
  evaluation.target_send_rate_hz =
      static_cast<double>(frequency_hz) * bounded_publishers;
  evaluation.target_receive_rate_hz =
      evaluation.target_send_rate_hz * bounded_subscribers;
  evaluation.achieved_send_rate_hz =
      static_cast<double>(measured_sent_messages) / measurement_seconds;
  evaluation.achieved_receive_rate_hz =
      static_cast<double>(measured_received_messages) / measurement_seconds;
  evaluation.achieved_send_ratio =
      evaluation.achieved_send_rate_hz / evaluation.target_send_rate_hz;
  evaluation.achieved_receive_ratio =
      evaluation.achieved_receive_rate_hz / evaluation.target_receive_rate_hz;

  constexpr double kComparisonEpsilon = 1e-12;
  evaluation.accepted =
      evaluation.achieved_send_ratio + kComparisonEpsilon >=
          evaluation.min_achieved_rate_ratio &&
      evaluation.achieved_receive_ratio + kComparisonEpsilon >=
          evaluation.min_achieved_rate_ratio;
  return evaluation;
}

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // TESTS_PERF_TEST_BENCHMARK_RATE_ACCEPTANCE_H_

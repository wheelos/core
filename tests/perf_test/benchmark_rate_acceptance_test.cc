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

#include "tests/perf_test/benchmark_rate_acceptance.h"

#include "gtest/gtest.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

TEST(BenchmarkRateAcceptanceTest, AdjustsRatesForFanoutTopology) {
  const auto result = EvaluateAchievedRates(10, 1, 4, 2, 20, 80, 0.95);

  EXPECT_DOUBLE_EQ(result.target_send_rate_hz, 10.0);
  EXPECT_DOUBLE_EQ(result.target_receive_rate_hz, 40.0);
  EXPECT_DOUBLE_EQ(result.achieved_send_ratio, 1.0);
  EXPECT_DOUBLE_EQ(result.achieved_receive_ratio, 1.0);
  EXPECT_TRUE(result.accepted);
}

TEST(BenchmarkRateAcceptanceTest, AdjustsRatesForPublisherScalingTopology) {
  const auto result = EvaluateAchievedRates(10, 3, 1, 2, 60, 60, 0.95);

  EXPECT_DOUBLE_EQ(result.target_send_rate_hz, 30.0);
  EXPECT_DOUBLE_EQ(result.target_receive_rate_hz, 30.0);
  EXPECT_TRUE(result.accepted);
}

TEST(BenchmarkRateAcceptanceTest, LowRateUsesConfiguredMeasurementWindow) {
  const auto result = EvaluateAchievedRates(1, 1, 1, 1, 1, 1, 0.95);

  EXPECT_DOUBLE_EQ(result.achieved_send_rate_hz, 1.0);
  EXPECT_DOUBLE_EQ(result.achieved_receive_rate_hz, 1.0);
  EXPECT_TRUE(result.accepted);
}

TEST(BenchmarkRateAcceptanceTest, RejectsEitherRateBelowConfiguredRatio) {
  const auto send_short = EvaluateAchievedRates(100, 1, 1, 1, 94, 100, 0.95);
  const auto receive_short =
      EvaluateAchievedRates(100, 1, 1, 1, 100, 94, 0.95);

  EXPECT_FALSE(send_short.accepted);
  EXPECT_FALSE(receive_short.accepted);
  EXPECT_DOUBLE_EQ(send_short.achieved_send_ratio, 0.94);
  EXPECT_DOUBLE_EQ(receive_short.achieved_receive_ratio, 0.94);
}

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

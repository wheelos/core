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

#include "tests/perf_test/fanout_validation.h"

#include "gtest/gtest.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {
namespace {

FanoutSubscriberValidation PassingValidation() {
  FanoutSubscriberValidation validation;
  validation.subscriber_index = 2;
  validation.endpoint_ready = true;
  validation.warmup_confirmed = true;
  validation.measured_delivery_confirmed = true;
  validation.shutdown_confirmed = true;
  validation.sent_messages = 100;
  validation.measured_received_messages = 100;
  validation.received_messages = 100;
  validation.final_drained_received_messages = 100;
  return validation;
}

TEST(FanoutValidationTest, AcceptsIndependentlyHealthySubscriber) {
  const auto validation =
      ValidateFanoutSubscriber(PassingValidation(), 0.01);
  EXPECT_TRUE(validation.success);
  EXPECT_DOUBLE_EQ(validation.loss_rate, 0.0);
}

TEST(FanoutValidationTest, RejectsAnyMissingLifecycleConfirmation) {
  auto validation = PassingValidation();
  validation.warmup_confirmed = false;
  EXPECT_FALSE(ValidateFanoutSubscriber(validation, 0.01).success);

  validation = PassingValidation();
  validation.measured_delivery_confirmed = false;
  EXPECT_FALSE(ValidateFanoutSubscriber(validation, 0.01).success);

  validation = PassingValidation();
  validation.shutdown_confirmed = false;
  EXPECT_FALSE(ValidateFanoutSubscriber(validation, 0.01).success);
}

TEST(FanoutValidationTest,
     CooldownOnlyDeliveryDoesNotConfirmMeasuredReadiness) {
  auto validation = PassingValidation();
  validation.measured_delivery_confirmed = false;
  validation.measured_received_messages = 0;
  validation.received_messages = 100;
  validation.final_drained_received_messages = 100;
  validation.total_loss = 0;

  EXPECT_FALSE(ValidateFanoutSubscriber(validation, 0.01).success);
}

TEST(FanoutValidationTest, RejectsSubscriberSpecificReliabilityFailure) {
  auto validation = PassingValidation();
  validation.total_loss = 2;
  validation.received_messages = 98;
  EXPECT_FALSE(ValidateFanoutSubscriber(validation, 0.01).success);

  validation = PassingValidation();
  validation.duplicates = 1;
  EXPECT_FALSE(ValidateFanoutSubscriber(validation, 0.01).success);

  validation = PassingValidation();
  validation.reordered = 1;
  EXPECT_FALSE(ValidateFanoutSubscriber(validation, 0.01).success);
}

TEST(FanoutValidationTest, AggregateFailsWhenAnySubscriberFails) {
  const auto passing =
      ValidateFanoutSubscriber(PassingValidation(), 0.01);
  auto missing_warmup = PassingValidation();
  missing_warmup.subscriber_index = 3;
  missing_warmup.warmup_confirmed = false;
  const auto failing =
      ValidateFanoutSubscriber(missing_warmup, 0.01);

  EXPECT_TRUE(AllFanoutSubscribersPass({passing, passing}));
  EXPECT_FALSE(AllFanoutSubscribersPass({passing, failing}));
  EXPECT_FALSE(AllFanoutSubscribersPass({}));
}

TEST(FanoutValidationTest, WarmupMatrixRejectsMissingPublisherReceipt) {
  const std::vector<std::vector<bool>> complete = {{true, true}};
  const std::vector<std::vector<bool>> missing_publisher = {{true, false}};

  EXPECT_TRUE(WarmupMatrixConfirmed(complete, 1, 2));
  EXPECT_FALSE(WarmupMatrixConfirmed(missing_publisher, 1, 2));
  EXPECT_EQ(MissingWarmupDiagnostics(missing_publisher, 1, 2),
            "missing subscriber/publisher warmups=s0:p1");
}

TEST(FanoutValidationTest, OrderingRejectsDuplicatesAndReordering) {
  EXPECT_TRUE(OrderingAccepted(0, 0));
  EXPECT_FALSE(OrderingAccepted(1, 0));
  EXPECT_FALSE(OrderingAccepted(0, 1));
}

}  // namespace
}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

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

#ifndef TESTS_PERF_TEST_FANOUT_VALIDATION_H_
#define TESTS_PERF_TEST_FANOUT_VALIDATION_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

struct FanoutSubscriberValidation {
  int subscriber_index = 0;
  bool endpoint_ready = false;
  bool warmup_confirmed = false;
  bool measured_delivery_confirmed = false;
  bool shutdown_confirmed = false;
  uint64_t sent_messages = 0;
  uint64_t measured_received_messages = 0;
  uint64_t received_messages = 0;
  uint64_t final_drained_received_messages = 0;
  uint64_t total_loss = 0;
  uint64_t max_consecutive_loss = 0;
  uint64_t duplicates = 0;
  uint64_t reordered = 0;
  double loss_rate = 0.0;
  bool success = false;
  std::string error_message;
};

inline FanoutSubscriberValidation ValidateFanoutSubscriber(
    FanoutSubscriberValidation validation, double max_loss_rate) {
  validation.loss_rate =
      validation.sent_messages == 0
          ? 0.0
          : static_cast<double>(validation.total_loss) /
                static_cast<double>(validation.sent_messages);
  validation.success =
      validation.endpoint_ready && validation.warmup_confirmed &&
      validation.measured_delivery_confirmed &&
      validation.shutdown_confirmed && validation.sent_messages > 0 &&
      validation.measured_received_messages > 0 &&
      validation.received_messages > 0 &&
      validation.duplicates == 0 && validation.reordered == 0 &&
      validation.loss_rate <= std::max(0.0, max_loss_rate);
  if (!validation.success) {
    validation.error_message =
        "subscriber " + std::to_string(validation.subscriber_index) +
        " failed fanout acceptance";
  }
  return validation;
}

inline bool AllFanoutSubscribersPass(
    const std::vector<FanoutSubscriberValidation>& validations) {
  return !validations.empty() &&
         std::all_of(validations.begin(), validations.end(),
                     [](const FanoutSubscriberValidation& validation) {
                       return validation.success;
                     });
}

inline bool WarmupMatrixConfirmed(
    const std::vector<std::vector<bool>>& warmup_received,
    size_t expected_subscribers, size_t expected_publishers) {
  if (expected_subscribers == 0 || expected_publishers == 0 ||
      warmup_received.size() != expected_subscribers) {
    return false;
  }
  return std::all_of(
      warmup_received.begin(), warmup_received.end(),
      [expected_publishers](const std::vector<bool>& subscriber_warmups) {
        return subscriber_warmups.size() == expected_publishers &&
               std::all_of(subscriber_warmups.begin(),
                           subscriber_warmups.end(),
                           [](bool received) { return received; });
      });
}

inline std::string MissingWarmupDiagnostics(
    const std::vector<std::vector<bool>>& warmup_received,
    size_t expected_subscribers, size_t expected_publishers) {
  std::ostringstream diagnostic;
  diagnostic << "missing subscriber/publisher warmups=";
  bool first = true;
  for (size_t subscriber = 0; subscriber < expected_subscribers;
       ++subscriber) {
    for (size_t publisher = 0; publisher < expected_publishers; ++publisher) {
      if (subscriber < warmup_received.size() &&
          publisher < warmup_received[subscriber].size() &&
          warmup_received[subscriber][publisher]) {
        continue;
      }
      diagnostic << (first ? "" : ",") << "s" << subscriber << ":p"
                 << publisher;
      first = false;
    }
  }
  if (first) {
    diagnostic << "none";
  }
  return diagnostic.str();
}

inline bool OrderingAccepted(uint64_t duplicates, uint64_t reordered) {
  return duplicates == 0 && reordered == 0;
}

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // TESTS_PERF_TEST_FANOUT_VALIDATION_H_

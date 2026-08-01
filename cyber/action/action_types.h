/******************************************************************************
 * Copyright 2026 WheelOS Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *****************************************************************************/

#ifndef CYBER_ACTION_ACTION_TYPES_H_
#define CYBER_ACTION_ACTION_TYPES_H_

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace apollo {
namespace cyber {

enum class ActionGoalState : std::uint8_t {
  UNKNOWN = 0,
  ACCEPTED,
  EXECUTING,
  CANCELING,
  SUCCEEDED,
  CANCELLED,
  ABORTED,
};

enum class ActionPreemptionPolicy : std::uint8_t {
  REJECT_NEW = 0,
  CANCEL_PREVIOUS,
  ALLOW_CONCURRENT,
};

struct ActionOptions {
  std::size_t max_active_goals = 1;
  std::uint32_t concurrency = 1;
  std::chrono::nanoseconds default_timeout = std::chrono::minutes(5);
  ActionPreemptionPolicy preemption_policy =
      ActionPreemptionPolicy::REJECT_NEW;
};

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_ACTION_ACTION_TYPES_H_

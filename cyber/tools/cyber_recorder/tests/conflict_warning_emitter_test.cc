// Copyright 2026 WheelOS All Rights Reserved.
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

#include "cyber/tools/cyber_recorder/record/core/conflict_warning_emitter.h"

#include "gtest/gtest.h"

namespace apollo {
namespace cyber {
namespace record {
namespace {

TEST(ConflictWarningEmitterTest, DeduplicatesPerWarningKind) {
  ConflictWarningEmitter emitter(7);

  EXPECT_TRUE(emitter.WarnSubscriptionConflict(
      "/camera/front/image", {"include_camera"}, {"exclude_camera"}));
  EXPECT_FALSE(emitter.WarnSubscriptionConflict(
      "/camera/front/image", {"include_camera"}, {"exclude_camera"}));

  EXPECT_TRUE(emitter.WarnPolicyConflict("/camera/front/image", "policy_a",
                                         {"policy_b"}));
  EXPECT_FALSE(emitter.WarnPolicyConflict("/camera/front/image", "policy_a",
                                          {"policy_b"}));
}

}  // namespace
}  // namespace record
}  // namespace cyber
}  // namespace apollo

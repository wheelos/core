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

#include "gtest/gtest.h"

#include "cyber/init.h"
#include "cyber/state.h"

namespace apollo {
namespace cyber {

TEST(CyberLifecycleIntegrationTest, InitAndClearFollowOneWayStateMachine) {
  EXPECT_EQ(GetState(), STATE_UNINITIALIZED);
  ASSERT_TRUE(Init("cyber_lifecycle_test"));
  EXPECT_TRUE(OK());
  EXPECT_FALSE(Init("cyber_lifecycle_test"));

  Clear();
  EXPECT_EQ(GetState(), STATE_SHUTDOWN);
  EXPECT_TRUE(IsShutdown());

  Clear();
  EXPECT_EQ(GetState(), STATE_SHUTDOWN);
  EXPECT_FALSE(Init("cyber_lifecycle_test"));
}

}  // namespace cyber
}  // namespace apollo

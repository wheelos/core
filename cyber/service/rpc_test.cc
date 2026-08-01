/******************************************************************************
 * Copyright 2026 WheelOS Authors. All Rights Reserved.
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

#include "cyber/service/rpc_context.h"

#include <atomic>
#include <chrono>
#include <memory>

#include "cyber/service/rpc_status.h"
#include "gtest/gtest.h"

namespace apollo {
namespace cyber {

TEST(RpcContextTest, DeadlineAndCancellation) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  RpcContext context(RpcContext::Clock::now() + std::chrono::milliseconds(20),
                     cancelled);
  EXPECT_TRUE(context.HasDeadline());
  EXPECT_FALSE(context.IsCancelled());
  EXPECT_GT(context.RemainingTime(), std::chrono::nanoseconds::zero());

  context.Cancel();
  EXPECT_TRUE(context.IsCancelled());

  RpcContext expired(
      RpcContext::Clock::now() - std::chrono::milliseconds(1),
      std::make_shared<std::atomic<bool>>(false));
  EXPECT_TRUE(expired.IsCancelled());
  EXPECT_EQ(expired.RemainingTime(), std::chrono::nanoseconds::zero());
}

TEST(RpcStatusTest, ResultCarriesStatusAndResponse) {
  auto response = std::make_shared<int>(42);
  RpcResult<int> success(response);
  EXPECT_TRUE(success.ok());
  EXPECT_EQ(*success.response(), 42);

  RpcResult<int> failure(
      RpcStatus(RpcStatusCode::UNAVAILABLE, "service unavailable"));
  EXPECT_FALSE(failure.ok());
  EXPECT_EQ(failure.status().code(), RpcStatusCode::UNAVAILABLE);
  EXPECT_EQ(failure.status().message(), "service unavailable");
}

}  // namespace cyber
}  // namespace apollo

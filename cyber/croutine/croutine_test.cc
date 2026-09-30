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
#include "cyber/croutine/croutine.h"

#include <chrono>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "cyber/common/global_data.h"
#include "cyber/cyber.h"
#include "cyber/init.h"
#include "cyber/metrics/metrics.h"

namespace apollo {
namespace cyber {
namespace croutine {

void function() { CRoutine::Yield(RoutineState::IO_WAIT); }

TEST(Croutine, croutinetest) {
  apollo::cyber::Init("croutine_test");
  std::shared_ptr<CRoutine> cr = std::make_shared<CRoutine>(function);
  auto id = GlobalData::RegisterTaskName("croutine");
  cr->set_id(id);
  cr->set_name("croutine");
  cr->set_processor_id(0);
  cr->set_priority(1);
  cr->set_state(RoutineState::DATA_WAIT);
  EXPECT_EQ(cr->state(), RoutineState::DATA_WAIT);
  cr->Wake();
  EXPECT_EQ(cr->state(), RoutineState::READY);
  cr->UpdateState();
  EXPECT_EQ(cr->state(), RoutineState::READY);
  EXPECT_EQ(*(cr->GetMainStack()), nullptr);
  cr->Resume();
  EXPECT_NE(*(cr->GetMainStack()), nullptr);
  EXPECT_EQ(cr->state(), RoutineState::IO_WAIT);
  cr->Stop();
  EXPECT_EQ(cr->Resume(), RoutineState::FINISHED);
}

TEST(Croutine, SchedulingNotificationCoalescesUntilResume) {
  auto& registry = metrics::Registry::Instance();
  registry.Configure(metrics::Mode::Basic);
  auto endpoint = registry.RegisterTask("coalesced");
  ASSERT_TRUE(endpoint);
  CRoutine routine([] {});
  routine.set_scheduling_metric(endpoint);
  routine.RecordSchedulingResume();
  EXPECT_EQ(endpoint->Snapshot().scheduling_latency.count, 0);
  routine.MarkSchedulingReady();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  routine.MarkSchedulingReady();
  routine.RecordSchedulingResume();
  const auto sample = endpoint->Snapshot().scheduling_latency;
  ASSERT_EQ(sample.count, 1);
  ASSERT_TRUE(sample.max_ns);
  EXPECT_GE(*sample.max_ns, 2000000);
  routine.RecordSchedulingResume();
  EXPECT_EQ(endpoint->Snapshot().scheduling_latency.count, 1);
  registry.Configure(metrics::Mode::Off);
  routine.MarkSchedulingReady();
  routine.RecordSchedulingResume();
  EXPECT_EQ(endpoint->Snapshot().scheduling_latency.count, 1);
}

TEST(Croutine, StoppedTaskDoesNotReportSchedulingResume) {
  auto& registry = metrics::Registry::Instance();
  registry.Configure(metrics::Mode::Basic);
  auto endpoint = registry.RegisterTask("stopped");
  ASSERT_TRUE(endpoint);
  CRoutine routine([] {});
  routine.set_scheduling_metric(endpoint);
  routine.MarkSchedulingReady();
  routine.Stop();
  EXPECT_EQ(routine.Resume(), RoutineState::FINISHED);
  EXPECT_EQ(endpoint->Snapshot().scheduling_count, 0);
  registry.Configure(metrics::Mode::Off);
}

TEST(Croutine, ConcurrentNotificationsProduceOneWaitSample) {
  auto& registry = metrics::Registry::Instance();
  registry.Configure(metrics::Mode::Basic);
  auto endpoint = registry.RegisterTask("concurrent_notify");
  ASSERT_TRUE(endpoint);
  CRoutine routine([] {});
  routine.set_scheduling_metric(endpoint);
  std::vector<std::thread> notifiers;
  for (int i = 0; i < 8; ++i) {
    notifiers.emplace_back([&] { routine.MarkSchedulingReady(); });
  }
  for (auto& notifier : notifiers) notifier.join();
  routine.RecordSchedulingResume();
  EXPECT_EQ(endpoint->Snapshot().scheduling_count, 1);
  registry.Configure(metrics::Mode::Off);
}

}  // namespace croutine
}  // namespace cyber
}  // namespace apollo

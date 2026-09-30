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

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "cyber/proto/unit_test.pb.h"

#include "cyber/common/global_data.h"
#include "cyber/cyber.h"
#include "cyber/component/component.h"
#include "cyber/metrics/metrics.h"
#include "cyber/scheduler/policy/choreography_context.h"
#include "cyber/scheduler/processor.h"
#include "cyber/scheduler/scheduler.h"

namespace apollo {
namespace cyber {
namespace {

using metrics::EndpointSnapshot;
using metrics::Kind;
using metrics::Registry;
using namespace std::chrono_literals;

template <typename Predicate>
bool WaitUntil(Predicate&& predicate, metrics::TimePoint deadline) {
  while (metrics::Clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::yield();
  }
  return predicate();
}

std::vector<EndpointSnapshot> ChannelMetrics(const std::string& channel) {
  std::vector<EndpointSnapshot> result;
  for (const auto& entry : Registry::Instance().Snapshot().endpoints) {
    if (entry.channel == channel) result.push_back(entry);
  }
  return result;
}

EndpointSnapshot Find(const std::vector<EndpointSnapshot>& endpoints, Kind kind,
                      const std::string& consumer = "") {
  for (const auto& entry : endpoints) {
    if (entry.kind == kind &&
        (consumer.empty() || entry.consumer == consumer)) {
      return entry;
    }
  }
  ADD_FAILURE() << "Missing runtime metric endpoint";
  return {};
}

class CallbackGate {
 public:
  void Invoke() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++count_;
    cv_.notify_all();
    if (count_ == 1) {
      cv_.wait(lock, [&] { return released_; });
    }
  }

  bool WaitForCount(size_t count, metrics::TimePoint deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_until(lock, deadline, [&] { return count_ >= count; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  size_t count_ = 0;
  bool released_ = false;
};

class BlockingMetricsComponent final : public Component<proto::UnitTest,
                                                         proto::UnitTest> {
 public:
  explicit BlockingMetricsComponent(std::shared_ptr<CallbackGate> gate)
      : gate_(std::move(gate)) {}

 private:
  bool Init() override { return true; }

  bool Proc(const std::shared_ptr<proto::UnitTest>&,
            const std::shared_ptr<proto::UnitTest>&) override {
    gate_->Invoke();
    return true;
  }

  std::shared_ptr<CallbackGate> gate_;
};

class RuntimeMetricsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Registry::Instance().Configure(metrics::Mode::Basic);
  }
  void TearDown() override {
    Registry::Instance().Configure(metrics::Mode::Off);
  }
};

TEST_F(RuntimeMetricsTest, SharedReceiverCountsOnceAndSlowConsumerShowsDrops) {
  const auto suffix = std::to_string(getpid());
  const std::string channel = "/runtime_metrics_" + suffix;
  const std::string slow_name = "metrics_slow_" + suffix;
  const std::string fast_name = "metrics_fast_" + suffix;
  auto slow_node = CreateNode(slow_name);
  auto fast_node = CreateNode(fast_name);
  auto writer_node = CreateNode("metrics_writer_" + suffix);
  ASSERT_TRUE(slow_node);
  ASSERT_TRUE(fast_node);
  ASSERT_TRUE(writer_node);

  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
  std::vector<std::string> slow_messages;
  ReaderConfig slow_config;
  slow_config.channel_name = channel;
  slow_config.pending_queue_size = 3;
  auto slow_reader = slow_node->CreateReader<proto::UnitTest>(
      slow_config, [&](const std::shared_ptr<proto::UnitTest>& msg) {
        std::unique_lock<std::mutex> lock(mutex);
        slow_messages.push_back(msg->case_name());
        if (slow_messages.size() == 1) {
          entered = true;
          cv.notify_all();
          cv.wait(lock, [&] { return release; });
        }
        cv.notify_all();
      });
  ASSERT_TRUE(slow_reader);
  auto fast_reader = fast_node->CreateReader<proto::UnitTest>(
      channel, [](const std::shared_ptr<proto::UnitTest>&) {});
  ASSERT_TRUE(fast_reader);
  auto writer = writer_node->CreateWriter<proto::UnitTest>(channel);
  ASSERT_TRUE(writer);

  const auto deadline = metrics::Clock::now() + 8s;
  ASSERT_TRUE(WaitUntil(
      [&] {
        std::vector<proto::RoleAttributes> readers;
        writer->GetReaders(&readers);
        return readers.size() >= 2;
      },
      deadline));
  const auto initial_metrics = ChannelMetrics(channel);
  const auto publish_window =
      Find(initial_metrics, Kind::Writer).publish_latency.window_start_mono_ns;
  auto publish = [&](int sequence) {
    auto message = std::make_shared<proto::UnitTest>();
    message->set_case_name(std::to_string(sequence));
    return writer->Write(message);
  };
  ASSERT_TRUE(publish(1));
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_until(lock, deadline, [&] { return entered; })) {
      release = true;
      lock.unlock();
      cv.notify_all();
      FAIL() << "Slow callback never started";
    }
  }

  bool all_sent = true;
  for (int i = 2; i <= 8; ++i) all_sent = publish(i) && all_sent;
  const bool delivered = WaitUntil(
      [&] {
        return Find(ChannelMetrics(channel), Kind::Receiver).receive_count == 8;
      },
      deadline);
  auto before = ChannelMetrics(channel);
  const auto slow_id = slow_name + "_" + channel;
  const auto blocked = Find(before, Kind::Consumer, slow_id);

  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  const bool completed = WaitUntil(
      [&] {
        return Find(ChannelMetrics(channel), Kind::Consumer, slow_id)
                   .callback_completed_count >= 2;
      },
      deadline);
  const auto after = ChannelMetrics(channel);
  EXPECT_TRUE(all_sent);
  EXPECT_TRUE(delivered);
  EXPECT_TRUE(completed);
  EXPECT_EQ(Find(after, Kind::Writer).publish_count, 8);
  const auto publish_samples = Find(after, Kind::Writer).publish_latency;
  if (publish_samples.window_start_mono_ns == publish_window) {
    EXPECT_EQ(publish_samples.count, 8);
  } else {
    EXPECT_LE(publish_samples.count, 8);
  }
  EXPECT_EQ(Find(after, Kind::Receiver).receive_count, 8);
  EXPECT_EQ(blocked.callback_inflight, 1);
  EXPECT_EQ(blocked.dequeue_count, 1);
  EXPECT_EQ(blocked.queue_depth, 3);
  EXPECT_EQ(blocked.queue_high_watermark, 3);
  EXPECT_EQ(blocked.drop_count[0], 4);
  const auto slow = Find(after, Kind::Consumer, slow_id);
  EXPECT_EQ(slow.enqueue_count, 8);
  EXPECT_EQ(slow.dequeue_count, 2);
  EXPECT_EQ(slow.drop_count[0], 4);
  EXPECT_EQ(slow.drop_count[1], 2);
  EXPECT_EQ(slow.queue_depth, 0);
  EXPECT_EQ(slow.callback_count, 2);
  EXPECT_EQ(slow.callback_error_count, 0);
  if (slow.queue_latency.window_start_mono_ns ==
      blocked.queue_latency.window_start_mono_ns) {
    EXPECT_EQ(slow.queue_latency.count, 2);
  } else {
    EXPECT_LE(slow.queue_latency.count, 2);
  }
  EXPECT_LE(slow.callback_latency.count, 2);
  {
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(slow_messages.size(), 2);
    EXPECT_EQ(slow_messages[0], "1");
    EXPECT_EQ(slow_messages[1], "8");
  }
}

TEST_F(RuntimeMetricsTest, ReaderShutdownCountsUnreadQueueAfterInflightCallback) {
  const auto suffix = std::to_string(getpid());
  const std::string channel = "/runtime_metrics_reader_shutdown_" + suffix;
  auto reader_node = CreateNode("metrics_reader_shutdown_" + suffix);
  auto writer_node = CreateNode("metrics_writer_shutdown_" + suffix);
  ASSERT_TRUE(reader_node);
  ASSERT_TRUE(writer_node);

  auto gate = std::make_shared<CallbackGate>();
  ReaderConfig config;
  config.channel_name = channel;
  config.pending_queue_size = 8;
  auto reader = reader_node->CreateReader<proto::UnitTest>(
      config, [gate](const std::shared_ptr<proto::UnitTest>&) {
        gate->Invoke();
      });
  auto writer = writer_node->CreateWriter<proto::UnitTest>(channel);
  ASSERT_TRUE(reader);
  ASSERT_TRUE(writer);

  const auto deadline = metrics::Clock::now() + 10s;
  ASSERT_TRUE(WaitUntil(
      [&] {
        std::vector<proto::RoleAttributes> readers;
        writer->GetReaders(&readers);
        return !readers.empty();
      },
      deadline));
  auto publish = [&](int sequence) {
    auto message = std::make_shared<proto::UnitTest>();
    message->set_case_name(std::to_string(sequence));
    return writer->Write(message);
  };
  ASSERT_TRUE(publish(1));
  if (!gate->WaitForCount(1, deadline)) {
    gate->Release();
    FAIL() << "Reader callback did not start";
  }

  bool all_sent = true;
  for (int sequence = 2; sequence <= 5; ++sequence) {
    all_sent = publish(sequence) && all_sent;
  }
  const auto consumer_name = reader_node->Name() + "_" + channel;
  const bool queued = all_sent && WaitUntil(
                                      [&] {
                                        const auto endpoints =
                                            ChannelMetrics(channel);
                                        return Find(endpoints, Kind::Receiver)
                                                       .receive_count == 5 &&
                                               Find(endpoints, Kind::Consumer,
                                                    consumer_name)
                                                       .queue_depth == 4;
                                      },
                                      deadline);
  if (!queued) {
    gate->Release();
    reader->Shutdown();
    FAIL() << "Reader did not accumulate four messages before shutdown";
  }

  std::atomic<bool> shutdown_returned{false};
  std::thread shutdown_thread([&] {
    reader->Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(10ms);
  EXPECT_FALSE(shutdown_returned.load(std::memory_order_acquire));
  const auto during_shutdown = Find(ChannelMetrics(channel), Kind::Consumer,
                                   consumer_name);
  EXPECT_FALSE(during_shutdown.retired);
  EXPECT_EQ(during_shutdown.callback_inflight, 1);
  EXPECT_EQ(during_shutdown.queue_depth, 4);
  EXPECT_FALSE(during_shutdown.shutdown_discard_count);

  gate->Release();
  shutdown_thread.join();
  EXPECT_TRUE(shutdown_returned.load(std::memory_order_acquire));
  const auto shutdown_deadline = metrics::Clock::now() + 5s;
  const bool destroyed = WaitUntil(
      [&] {
        const auto endpoint =
            Find(ChannelMetrics(channel), Kind::Consumer, consumer_name);
        return endpoint.callback_inflight == 0 &&
               endpoint.shutdown_discard_count.has_value();
      },
      shutdown_deadline);
  EXPECT_TRUE(destroyed);
  const auto after_shutdown = Find(ChannelMetrics(channel), Kind::Consumer,
                                  consumer_name);
  EXPECT_TRUE(after_shutdown.retired);
  EXPECT_TRUE(after_shutdown.retired_at_unix_ns.has_value());
  EXPECT_EQ(after_shutdown.callback_completed_count, 1);
  EXPECT_EQ(after_shutdown.shutdown_discard_count.value_or(0), 4);
  EXPECT_EQ(after_shutdown.queue_depth, 0);
}

TEST_F(RuntimeMetricsTest,
       ComponentShutdownCountsUnreadFusedQueueWithInflightProc) {
  const auto suffix = std::to_string(getpid());
  const std::string component_name = "metrics_component_shutdown_" + suffix;
  const std::string primary_channel =
      "/runtime_metrics_component_primary_" + suffix;
  const std::string secondary_channel =
      "/runtime_metrics_component_secondary_" + suffix;
  const std::string consumer_name = component_name + ":component";
  auto primary_writer_node = CreateNode("metrics_primary_writer_" + suffix);
  auto secondary_writer_node = CreateNode("metrics_secondary_writer_" + suffix);
  ASSERT_TRUE(primary_writer_node);
  ASSERT_TRUE(secondary_writer_node);

  auto gate = std::make_shared<CallbackGate>();
  auto component = std::make_shared<BlockingMetricsComponent>(gate);
  ComponentConfig config;
  config.set_name(component_name);
  auto* primary_reader = config.add_readers();
  primary_reader->set_channel(primary_channel);
  primary_reader->set_pending_queue_size(8);
  auto* secondary_reader = config.add_readers();
  secondary_reader->set_channel(secondary_channel);
  secondary_reader->set_pending_queue_size(8);
  ASSERT_TRUE(component->Initialize(config));

  auto primary_writer =
      primary_writer_node->CreateWriter<proto::UnitTest>(primary_channel);
  auto secondary_writer =
      secondary_writer_node->CreateWriter<proto::UnitTest>(secondary_channel);
  ASSERT_TRUE(primary_writer);
  ASSERT_TRUE(secondary_writer);
  const auto deadline = metrics::Clock::now() + 10s;
  ASSERT_TRUE(WaitUntil(
      [&] {
        std::vector<proto::RoleAttributes> readers;
        primary_writer->GetReaders(&readers);
        if (readers.empty()) return false;
        readers.clear();
        secondary_writer->GetReaders(&readers);
        return !readers.empty();
      },
      deadline));

  auto message = std::make_shared<proto::UnitTest>();
  message->set_case_name("secondary");
  ASSERT_TRUE(secondary_writer->Write(message));
  message->set_case_name("primary");
  ASSERT_TRUE(primary_writer->Write(message));
  if (!gate->WaitForCount(1, deadline)) {
    gate->Release();
    component->Shutdown();
    FAIL() << "Component Proc did not start";
  }

  bool all_sent = true;
  for (int sequence = 2; sequence <= 4; ++sequence) {
    message = std::make_shared<proto::UnitTest>();
    message->set_case_name(std::to_string(sequence));
    all_sent = primary_writer->Write(message) && all_sent;
  }
  const bool queued = all_sent && WaitUntil(
                                      [&] {
                                        return Find(
                                                   ChannelMetrics(
                                                       primary_channel),
                                                   Kind::Consumer,
                                                   consumer_name)
                                                   .queue_depth == 3;
                                      },
                                      deadline);
  if (!queued) {
    gate->Release();
    component->Shutdown();
    FAIL() << "Component fusion queue did not accumulate three messages";
  }

  std::atomic<bool> shutdown_returned{false};
  std::thread shutdown_thread([&] {
    component->Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(10ms);
  EXPECT_FALSE(shutdown_returned.load(std::memory_order_acquire));
  const auto during_shutdown =
      Find(ChannelMetrics(primary_channel), Kind::Consumer, consumer_name);
  EXPECT_FALSE(during_shutdown.retired);
  EXPECT_EQ(during_shutdown.callback_inflight, 1);
  EXPECT_EQ(during_shutdown.queue_depth, 3);
  EXPECT_FALSE(during_shutdown.shutdown_discard_count);

  gate->Release();
  shutdown_thread.join();
  EXPECT_TRUE(shutdown_returned.load(std::memory_order_acquire));
  const auto shutdown_deadline = metrics::Clock::now() + 5s;
  const bool destroyed = WaitUntil(
      [&] {
        const auto endpoint =
            Find(ChannelMetrics(primary_channel), Kind::Consumer,
                 consumer_name);
        return endpoint.callback_inflight == 0 &&
               endpoint.shutdown_discard_count.has_value();
      },
      shutdown_deadline);
  EXPECT_TRUE(destroyed);
  const auto after_shutdown =
      Find(ChannelMetrics(primary_channel), Kind::Consumer, consumer_name);
  EXPECT_TRUE(after_shutdown.retired);
  EXPECT_TRUE(after_shutdown.retired_at_unix_ns.has_value());
  EXPECT_EQ(after_shutdown.callback_completed_count, 1);
  EXPECT_EQ(after_shutdown.shutdown_discard_count.value_or(0), 3);
  EXPECT_EQ(after_shutdown.queue_depth, 0);
}

TEST_F(RuntimeMetricsTest, TaskNotificationRecordsOnlyReadyToResumeWait) {
  const auto name = "metrics_task_" + std::to_string(getpid());
  std::atomic<bool> entered{false};
  std::atomic<bool> resumed{false};
  auto* sched = scheduler::Instance();
  ASSERT_TRUE(sched->CreateTask(
      [&] {
        entered.store(true, std::memory_order_release);
        croutine::CRoutine::Yield(croutine::RoutineState::DATA_WAIT);
        resumed.store(true, std::memory_order_release);
      },
      name));
  const auto id = common::GlobalData::RegisterTaskName(name);
  const auto deadline = metrics::Clock::now() + 8s;
  ASSERT_TRUE(WaitUntil([&] { return entered.load(std::memory_order_acquire); },
                        deadline));
  while (!resumed.load(std::memory_order_acquire) &&
         metrics::Clock::now() < deadline) {
    EXPECT_TRUE(sched->NotifyTask(id));
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_TRUE(resumed.load(std::memory_order_acquire));
  bool found = false;
  for (const auto& endpoint : Registry::Instance().Snapshot().endpoints) {
    if (endpoint.kind != Kind::Task || endpoint.task != name) continue;
    found = true;
    EXPECT_EQ(endpoint.scheduling_count, 1);
    if (endpoint.scheduling_latency.count != 0) {
      ASSERT_TRUE(endpoint.scheduling_latency.max_ns);
      EXPECT_GT(*endpoint.scheduling_latency.max_ns, 0);
    }
  }
  EXPECT_TRUE(found);
  EXPECT_TRUE(sched->RemoveTask(name));
}

TEST_F(RuntimeMetricsTest, RunnableTaskWaitsBehindBlockedProcessor) {
  std::mutex mutex;
  std::condition_variable cv;
  bool release = false;
  std::atomic<bool> waiting{false}, blocked{false}, resumed{false};
  auto metric = Registry::Instance().RegisterTask("blocked_processor_task");
  ASSERT_TRUE(metric);
  auto context = std::make_shared<scheduler::ChoreographyContext>();
  auto processor = std::make_shared<scheduler::Processor>();
  processor->BindContext(context);
  auto target = std::make_shared<croutine::CRoutine>([&] {
    waiting.store(true, std::memory_order_release);
    croutine::CRoutine::Yield(croutine::RoutineState::DATA_WAIT);
    resumed.store(true, std::memory_order_release);
  });
  target->set_scheduling_metric(metric);
  auto blocker = std::make_shared<croutine::CRoutine>([&] {
    blocked.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, 2s, [&] { return release; });
  });
  blocker->set_priority(1);
  context->Enqueue(target);
  context->Notify();
  const auto deadline = metrics::Clock::now() + 5s;
  const bool started = WaitUntil(
      [&] { return waiting.load(std::memory_order_acquire); }, deadline);
  if (started) {
    context->Enqueue(blocker);
    context->Notify();
  }
  const bool blocker_started =
      started &&
      WaitUntil([&] { return blocked.load(std::memory_order_acquire); },
                deadline);
  if (blocker_started) {
    target->MarkSchedulingReady();
    target->SetUpdateFlag();
    context->Notify();
    std::this_thread::sleep_for(20ms);
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_one();
  const bool target_resumed =
      blocker_started &&
      WaitUntil([&] { return resumed.load(std::memory_order_acquire); },
                deadline);
  context->Shutdown();
  processor->Stop();
  EXPECT_TRUE(started);
  EXPECT_TRUE(blocker_started);
  EXPECT_TRUE(target_resumed);
  const auto sample = metric->Snapshot();
  EXPECT_EQ(sample.scheduling_count, blocker_started ? 1 : 0);
  if (sample.scheduling_latency.count != 0) {
    ASSERT_TRUE(sample.scheduling_latency.max_ns);
    EXPECT_GE(*sample.scheduling_latency.max_ns, 15'000'000ULL);
  }
}

}  // namespace
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  const auto cyber_path = (std::filesystem::current_path() / "cyber").string();
  setenv("CYBER_PATH", cyber_path.c_str(), 1);
  testing::InitGoogleTest(&argc, argv);
  if (!apollo::cyber::Init(argv[0])) return EXIT_FAILURE;
  const int result = RUN_ALL_TESTS();
  apollo::cyber::Clear();
  return result;
}

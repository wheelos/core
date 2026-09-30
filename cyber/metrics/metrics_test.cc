#include "cyber/metrics/metrics.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace apollo {
namespace cyber {
namespace metrics {
namespace {

using namespace std::chrono_literals;

TEST(MetricsTest, HistogramWindowAndPercentiles) {
  Histogram hist;
  const auto now = TimePoint(120s);
  auto empty = hist.Snapshot(now);
  EXPECT_EQ(empty.count, 0);
  EXPECT_FALSE(empty.p99_ns);
  EXPECT_FALSE(empty.max_ns);
  for (int i = 0; i < 1000; ++i) {
    const auto sample =
        1000ULL + static_cast<uint64_t>(i) * (60000000000ULL - 1000) / 999;
    hist.Observe(std::chrono::nanoseconds(sample), now);
  }
  const auto snapshot = hist.Snapshot(now);
  ASSERT_EQ(snapshot.count, 1000);
  EXPECT_EQ(snapshot.window_start_mono_ns, 120000000000ULL);
  EXPECT_EQ(snapshot.window_end_mono_ns, 180000000000ULL);
  auto near = [](uint64_t value, uint64_t expected) {
    EXPECT_LE(std::abs(static_cast<double>(value) / expected - 1.0), 0.02);
  };
  near(*snapshot.p50_ns, 1000ULL + 499ULL * (60000000000ULL - 1000) / 999);
  near(*snapshot.p90_ns, 1000ULL + 899ULL * (60000000000ULL - 1000) / 999);
  near(*snapshot.p99_ns, 1000ULL + 989ULL * (60000000000ULL - 1000) / 999);
  near(*snapshot.p999_ns, 1000ULL + 998ULL * (60000000000ULL - 1000) / 999);
  EXPECT_EQ(*snapshot.max_ns, 60000000000ULL);
  EXPECT_EQ(hist.Snapshot(now + 60s).count, 0);
  EXPECT_EQ(hist.Snapshot(now + 60s).window_start_mono_ns, 180000000000ULL);
  hist.Observe(0ns, now + 60s);
  hist.Observe(61s, now + 60s);
  EXPECT_EQ(hist.Snapshot(now + 60s).underflow_count, 1);
  EXPECT_EQ(hist.Snapshot(now + 60s).overflow_count, 1);
  EXPECT_EQ(*hist.Snapshot(now + 60s).max_ns, 61000000000ULL);
  hist.Observe(-1ns, now + 60s);
  EXPECT_EQ(hist.Snapshot(now + 60s).count, 2);
  Histogram extremes;
  extremes.Observe(1us, now);
  extremes.Observe(60s, now);
  near(*extremes.Snapshot(now).p50_ns, 1000ULL);
  near(*extremes.Snapshot(now).p99_ns, 60000000000ULL);
  Histogram single;
  single.Observe(1us, now);
  const auto single_snapshot = single.Snapshot(now);
  ASSERT_TRUE(single_snapshot.max_ns);
  EXPECT_EQ(single_snapshot.p50_ns, single_snapshot.max_ns);
  EXPECT_EQ(single_snapshot.p90_ns, single_snapshot.max_ns);
  EXPECT_EQ(single_snapshot.p99_ns, single_snapshot.max_ns);
  EXPECT_EQ(single_snapshot.p999_ns, single_snapshot.max_ns);
  Histogram split;
  for (int i = 0; i < 990; ++i) split.Observe(1us, now);
  for (int i = 0; i < 10; ++i) split.Observe(1s, now);
  const auto split_snapshot = split.Snapshot(now);
  EXPECT_EQ(split_snapshot.count, 1000);
  ASSERT_TRUE(split_snapshot.p50_ns);
  ASSERT_TRUE(split_snapshot.p90_ns);
  ASSERT_TRUE(split_snapshot.p99_ns);
  near(*split_snapshot.p50_ns, 1000);
  EXPECT_EQ(split_snapshot.p50_ns, split_snapshot.p90_ns);
  EXPECT_EQ(split_snapshot.p90_ns, split_snapshot.p99_ns);
  ASSERT_TRUE(split_snapshot.p999_ns);
  near(*split_snapshot.p999_ns, 1000000000ULL);
  EXPECT_LE(*split_snapshot.p999_ns, *split_snapshot.max_ns);
  Histogram below_minimum;
  below_minimum.Observe(0ns, now);
  EXPECT_EQ(below_minimum.Snapshot(now).p50_ns, 0);
  EXPECT_EQ(below_minimum.Snapshot(now).p999_ns, 0);
  Histogram above_maximum;
  above_maximum.Observe(61s, now);
  EXPECT_EQ(above_maximum.Snapshot(now).p50_ns, 60000000000ULL);
  EXPECT_EQ(above_maximum.Snapshot(now).p999_ns, 60000000000ULL);
}

TEST(MetricsTest, EndpointAccountingAndJsonEscaping) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto writer = registry.RegisterWriter("/a\"\\\n", "node");
  auto receiver = registry.RegisterReceiver("/a");
  auto consumer = registry.RegisterConsumer("/a", "consumer");
  ASSERT_TRUE(writer);
  ASSERT_TRUE(receiver);
  ASSERT_TRUE(consumer);
  const auto now = TimePoint(180s + 10s);
  writer->RecordPublish(true, now);
  writer->RecordPublish(false, now);
  receiver->RecordReceive(now);
  consumer->RecordEnqueue(3);
  consumer->RecordDequeue();
  consumer->RecordDrop(DropReason::OverflowOverwrite);
  consumer->SetQueueDepth(2, now);
  consumer->SetQueueDepth(1, now);
  consumer->CallbackStart();
  consumer->ObserveQueueLatency(2ms, now);
  consumer->ObserveCallbackLatency(5ms, now);
  auto in_flight = consumer->Snapshot(now);
  EXPECT_EQ(in_flight.callback_inflight, 1);
  consumer->CallbackComplete(true);
  auto s = consumer->Snapshot(now);
  EXPECT_EQ(s.enqueue_count, 3);
  EXPECT_EQ(s.dequeue_count, 1);
  EXPECT_EQ(s.drop_count[0], 1);
  EXPECT_EQ(s.queue_depth, 1);
  EXPECT_EQ(s.queue_high_watermark, 2);
  EXPECT_EQ(s.callback_count, 1);
  EXPECT_EQ(s.callback_completed_count, 1);
  EXPECT_EQ(s.callback_error_count, 1);
  EXPECT_EQ(s.callback_inflight, 0);
  EXPECT_EQ(s.queue_latency.count, 1);
  EXPECT_EQ(s.callback_latency.count, 1);
  EXPECT_EQ(writer->Snapshot(now).publish_count, 1);
  EXPECT_EQ(writer->Snapshot(now).publish_attempt_count, 2);
  EXPECT_FALSE(writer->Snapshot(now).publish_latency.p99_ns);
  EXPECT_EQ(receiver->Snapshot(now).receive_count, 1);
  EXPECT_NEAR(writer->Snapshot(now).publish_rate, 1.0 / 60, 0.0001);
  EXPECT_EQ(writer->Snapshot(now).rate_elapsed_seconds, 60.0);
  const auto json = registry.Snapshot(now).ToJson();
  EXPECT_NE(json.find("/a\\\"\\\\\\n"), std::string::npos);
  EXPECT_NE(json.find("\"window_start_mono_ns\":"), std::string::npos);
  EXPECT_NE(json.find("\"transport_latency\":{\"count\":0,\"p50_ns\":null,"
                      "\"p90_ns\":null,\"p99_ns\":null,\"p999_ns\":null,"
                      "\"max_ns\":null,\"reason\":\"not_instrumented\"}"),
            std::string::npos);
  EXPECT_EQ(consumer->Snapshot(now + 60s).queue_latency.count, 0);
  EXPECT_EQ(consumer->Snapshot(now + 60s).queue_window_high_watermark, 1);
}

TEST(MetricsTest, JsonEncodingPreservesNumbersAndControlCharacters) {
  ProcessSnapshot empty;
  empty.process_instance = "instance";
  EXPECT_EQ(empty.ToJson(),
            "{\"schema_version\":1,\"process_instance\":\"instance\","
            "\"generated_at_unix_ns\":0,\"mode\":\"off\",\"live_series\":0,"
            "\"rejected_registrations\":0,\"retired_snapshots_dropped\":0,"
            "\"endpoints\":[]}");

  ProcessSnapshot snapshot;
  snapshot.mode = Mode::Detailed;
  snapshot.process_instance = std::string("x\0\x01\x1f\"\\", 6);
  snapshot.live_series = 1;
  EndpointSnapshot writer;
  writer.kind = Kind::Writer;
  writer.channel = snapshot.process_instance;
  writer.publish_count = UINT64_MAX;
  writer.publish_rate = 1.0 / 60;
  writer.retired_at_unix_ns = UINT64_MAX;
  snapshot.endpoints.push_back(writer);
  const auto json = snapshot.ToJson();
  EXPECT_NE(json.find("\"mode\":\"detailed\""), std::string::npos);
  EXPECT_NE(json.find("x\\u0000\\u0001\\u001f\\\"\\\\"), std::string::npos);
  EXPECT_NE(json.find("\"publish_count\":18446744073709551615"),
            std::string::npos);
  EXPECT_NE(json.find("\"retired_at_unix_ns\":18446744073709551615"),
            std::string::npos);
  EXPECT_NE(json.find("\"publish_rate\":0.0166667"), std::string::npos);
}

TEST(MetricsTest, QueueWindowHighWatermarkIncludesCarriedBacklog) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto consumer = registry.RegisterConsumer("/backlog", "reader");
  ASSERT_TRUE(consumer);
  consumer->SetQueueDepth(3, TimePoint(239s));
  auto next = consumer->Snapshot(TimePoint(240s));
  EXPECT_EQ(next.queue_depth, 3);
  EXPECT_EQ(next.queue_window_high_watermark, 3);
  consumer->SetQueueDepth(1, TimePoint(241s));
  EXPECT_EQ(consumer->Snapshot(TimePoint(241s)).queue_window_high_watermark, 3);
  consumer->SetQueueDepth(4, TimePoint(242s));
  EXPECT_EQ(consumer->Snapshot(TimePoint(242s)).queue_window_high_watermark, 4);
  EXPECT_EQ(consumer->Snapshot(TimePoint(300s)).queue_window_high_watermark, 4);
  consumer->SetQueueDepth(0, TimePoint(301s));
  EXPECT_EQ(consumer->Snapshot(TimePoint(301s)).queue_window_high_watermark, 4);
  EXPECT_EQ(consumer->Snapshot(TimePoint(360s)).queue_window_high_watermark, 0);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, PublishDurationIsMeasuredOnlyWhenProvided) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto writer = registry.RegisterWriter("/duration", "writer");
  ASSERT_TRUE(writer);
  const auto now = TimePoint(240s + 3s);
  writer->RecordPublish(true, 2ms, now);
  writer->RecordPublish(false, 7ms, now);
  writer->RecordPublish(true, now);
  const auto snapshot = writer->Snapshot(now);
  EXPECT_EQ(snapshot.publish_attempt_count, 3);
  EXPECT_EQ(snapshot.publish_error_count, 1);
  EXPECT_EQ(snapshot.publish_latency.count, 2);
  ASSERT_TRUE(snapshot.publish_latency.max_ns);
  EXPECT_EQ(*snapshot.publish_latency.max_ns, 7000000);
  EXPECT_NE(
      registry.Snapshot(now).ToJson().find("\"publish_latency\":{\"count\":2"),
      std::string::npos);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, RateAndPublishWindowDoNotCarryOldSamples) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto writer = registry.RegisterWriter("/windows", "node");
  ASSERT_TRUE(writer);
  const auto first = TimePoint(60s + 59s);
  writer->RecordPublish(true, 1ms, first);
  const auto within = writer->Snapshot(first);
  EXPECT_EQ(within.publish_count, 1);
  EXPECT_EQ(within.publish_latency.count, 1);
  EXPECT_GT(within.publish_rate, 0);
  const auto next = writer->Snapshot(TimePoint(120s));
  EXPECT_EQ(next.publish_count, 1);
  EXPECT_EQ(next.publish_latency.count, 0);
  EXPECT_FALSE(next.publish_latency.p99_ns);
  EXPECT_NEAR(next.publish_rate, 1.0 / 60, 0.0001);
  writer->RecordPublish(false, 3ms, TimePoint(121s));
  const auto failed = writer->Snapshot(TimePoint(121s));
  EXPECT_EQ(failed.publish_count, 1);
  EXPECT_EQ(failed.publish_error_count, 1);
  EXPECT_EQ(failed.publish_latency.count, 1);
  EXPECT_EQ(*failed.publish_latency.max_ns, 3000000);
  EXPECT_EQ(writer->Snapshot(TimePoint(180s)).publish_rate, 0);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, RollingRateCrossesAlignedBoundaryAndExpires) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto writer = registry.RegisterWriter("/rolling-rate", "writer");
  auto receiver = registry.RegisterReceiver("/rolling-rate");
  ASSERT_TRUE(writer);
  ASSERT_TRUE(receiver);
  writer->RecordPublish(true, TimePoint(119s));
  receiver->RecordReceive(TimePoint(119s));
  writer->RecordPublish(false, TimePoint(120s));
  writer->RecordPublish(true, TimePoint(120s));
  EXPECT_NEAR(writer->Snapshot(TimePoint(120s)).publish_rate, 2.0 / 60, 1e-9);
  EXPECT_NEAR(receiver->Snapshot(TimePoint(120s)).receive_rate, 1.0 / 60, 1e-9);
  EXPECT_NEAR(writer->Snapshot(TimePoint(178s)).publish_rate, 2.0 / 60, 1e-9);
  EXPECT_NEAR(writer->Snapshot(TimePoint(179s)).publish_rate, 1.0 / 60, 1e-9);
  EXPECT_EQ(writer->Snapshot(TimePoint(180s)).publish_rate, 0);
  EXPECT_EQ(receiver->Snapshot(TimePoint(179s)).receive_rate, 0);
  EXPECT_EQ(writer->Snapshot(TimePoint(180s)).publish_count, 2);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, ConcurrentSnapshotsAndBoundedRegistration) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto endpoint = registry.RegisterConsumer("/concurrent", "callback");
  ASSERT_TRUE(endpoint);
  std::thread producer([&] {
    for (int i = 0; i < 1000; ++i) {
      endpoint->RecordEnqueue();
      endpoint->CallbackStart();
      endpoint->CallbackComplete();
    }
  });
  for (int i = 0; i < 100; ++i) {
    const auto snapshot = endpoint->Snapshot();
    EXPECT_LE(snapshot.callback_completed_count, snapshot.callback_count);
    registry.Snapshot();
  }
  producer.join();
  EXPECT_EQ(endpoint->Snapshot().enqueue_count, 1000);
  EXPECT_EQ(endpoint->Snapshot().callback_completed_count, 1000);

  std::vector<std::shared_ptr<Endpoint>> handles;
  handles.reserve(Registry::kMaxSeries);
  // The only other live series is endpoint; expired series are reclaimed.
  for (size_t i = 1; i < Registry::kMaxSeries; ++i) {
    auto handle = registry.RegisterReceiver("/bounded");
    ASSERT_TRUE(handle) << i;
    handles.push_back(std::move(handle));
  }
  const auto rejected = registry.Snapshot().rejected_registrations;
  EXPECT_FALSE(registry.RegisterReceiver("/bounded"));
  EXPECT_EQ(registry.Snapshot().rejected_registrations, rejected + 1);
  handles.clear();
  EXPECT_FALSE(registry.RegisterReceiver(std::string(257, 'x')));
  EXPECT_TRUE(registry.RegisterReceiver("/reclaimed"));
  registry.Configure(Mode::Off);
  EXPECT_FALSE(registry.RegisterWriter("/off", "node"));
  const auto before = endpoint->Snapshot().enqueue_count;
  endpoint->RecordEnqueue();
  EXPECT_EQ(endpoint->Snapshot().enqueue_count, before);
}

TEST(MetricsTest, ChurnReusesSeriesCapacity) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  const auto rejected = registry.Snapshot().rejected_registrations;
  for (size_t i = 0; i < Registry::kMaxSeries * 2; ++i) {
    auto endpoint = registry.RegisterConsumer("/churn", "reader");
    ASSERT_TRUE(endpoint) << i;
  }
  const auto snapshot = registry.Snapshot();
  EXPECT_EQ(snapshot.live_series, 0);
  EXPECT_EQ(snapshot.rejected_registrations, rejected);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, RetiredQueueSnapshotRemainsAvailable) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto endpoint = registry.RegisterConsumer("/retired", "reader");
  ASSERT_TRUE(endpoint);
  endpoint->RecordEnqueue(3);
  endpoint->SetQueueDepth(3);
  endpoint->RecordShutdownDiscard(2);
  endpoint->SetQueueDepth(0);
  registry.Retire(endpoint);

  const auto snapshot = registry.Snapshot();
  EXPECT_EQ(snapshot.live_series, 0);
  const auto found = std::find_if(
      snapshot.endpoints.begin(), snapshot.endpoints.end(),
      [](const EndpointSnapshot& item) {
        return item.channel == "/retired" && item.consumer == "reader";
      });
  ASSERT_NE(found, snapshot.endpoints.end());
  EXPECT_TRUE(found->retired);
  ASSERT_TRUE(found->retired_at_unix_ns);
  EXPECT_LE(*found->retired_at_unix_ns, snapshot.generated_at_unix_ns);
  EXPECT_EQ(found->enqueue_count, 3);
  EXPECT_EQ(found->queue_depth, 0);
  ASSERT_TRUE(found->shutdown_discard_count);
  EXPECT_EQ(*found->shutdown_discard_count, 2);
  const auto retired_at = *found->retired_at_unix_ns;
  auto replacement = registry.RegisterConsumer("/retired", "reader");
  ASSERT_TRUE(replacement);
  const auto later = registry.Snapshot(Clock::now() + 120s);
  EXPECT_EQ(later.live_series, 1);
  size_t retired_count = 0;
  size_t active_count = 0;
  for (const auto& item : later.endpoints) {
    if (item.channel != "/retired" || item.consumer != "reader") continue;
    if (item.retired) {
      ++retired_count;
      EXPECT_EQ(item.retired_at_unix_ns, retired_at);
      EXPECT_EQ(item.enqueue_count, 3);
    } else {
      ++active_count;
      EXPECT_FALSE(item.retired_at_unix_ns);
    }
  }
  EXPECT_EQ(retired_count, 1);
  EXPECT_EQ(active_count, 1);
  const auto json = later.ToJson();
  EXPECT_NE(json.find("\"retired\":true,\"retired_at_unix_ns\":" +
                      std::to_string(retired_at)),
            std::string::npos);
  EXPECT_NE(json.find("\"retired\":false,\"retired_at_unix_ns\":null"),
            std::string::npos);
  EXPECT_NE(json.find("\"retired_snapshots_dropped\":"),
            std::string::npos);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, RetiredSnapshotHistoryIsBounded) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  const auto before = registry.Snapshot();
  const auto existing_retired =
      before.endpoints.size() - before.live_series;
  const auto to_retire =
      Registry::kMaxRetiredSnapshots - existing_retired + 1;
  for (size_t i = 0; i < to_retire; ++i) {
    auto endpoint =
        registry.RegisterConsumer("/retired-bound", std::to_string(i));
    ASSERT_TRUE(endpoint);
    endpoint->RecordShutdownDiscard(i);
    registry.Retire(endpoint);
  }

  const auto after = registry.Snapshot();
  EXPECT_EQ(after.live_series, 0);
  EXPECT_EQ(after.endpoints.size(), Registry::kMaxRetiredSnapshots);
  EXPECT_EQ(after.retired_snapshots_dropped,
            before.retired_snapshots_dropped + 1);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, TaskLatencyHasSeparateIdentityAndWindow) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto task = registry.RegisterTask("reader_task");
  ASSERT_TRUE(task);
  const auto now = TimePoint(180s + 10s);
  task->ObserveSchedulingLatency(3ms, now);
  const auto snapshot = task->Snapshot(now);
  EXPECT_EQ(snapshot.kind, Kind::Task);
  EXPECT_EQ(snapshot.task, "reader_task");
  EXPECT_TRUE(snapshot.channel.empty());
  EXPECT_TRUE(snapshot.consumer.empty());
  EXPECT_EQ(snapshot.scheduling_count, 1);
  EXPECT_EQ(snapshot.scheduling_latency.count, 1);
  EXPECT_EQ(*snapshot.scheduling_latency.max_ns, 3000000);
  EXPECT_EQ(task->Snapshot(now + 60s).scheduling_latency.count, 0);
  EXPECT_EQ(task->Snapshot(now + 60s).scheduling_count, 1);
  const auto json = registry.Snapshot(now).ToJson();
  EXPECT_NE(json.find("\"kind\":\"task\",\"channel\":\"\",\"node\":\"\","
                      "\"consumer\":\"\",\"task\":\"reader_task\""),
            std::string::npos);
  EXPECT_NE(json.find("\"scheduling_latency\":{\"count\":1"),
            std::string::npos);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, PrivateAtomicFileExport) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  char directory[] = "/tmp/cyber-metrics-test-XXXXXX";
  ASSERT_NE(mkdtemp(directory), nullptr);
  const std::string path = std::string(directory) + "/snapshot.json";
  EXPECT_FALSE(registry.StartExport("/tmp/cyber-metrics-public.json"));
  auto endpoint = registry.RegisterWriter("/export", "writer");
  ASSERT_TRUE(endpoint);
  ASSERT_TRUE(registry.StartExport(path));
  endpoint->RecordPublish(true);
  registry.StopExport();
  std::ifstream input(path);
  ASSERT_TRUE(input.good());
  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  EXPECT_NE(json.find("\"generated_at_unix_ns\":"), std::string::npos);
  EXPECT_NE(json.find("\"channel\":\"/export\""), std::string::npos);
  EXPECT_NE(json.find("\"publish_count\":1"), std::string::npos);
  input.close();
  EXPECT_EQ(std::remove(path.c_str()), 0);
  EXPECT_EQ(rmdir(directory), 0);
  registry.Configure(Mode::Off);
}

TEST(MetricsTest, CallbackFailuresPreserveBehavior) {
  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  auto endpoint = registry.RegisterConsumer("/callback", "component");
  ASSERT_TRUE(endpoint);
  EXPECT_FALSE(MeasureCallback(endpoint, [] { return false; }));
  EXPECT_THROW(MeasureCallback(endpoint,
                               []() -> void {
                                 throw std::runtime_error("callback failed");
                               }),
               std::runtime_error);
  MeasureCallback(endpoint, [] {});
  const auto snapshot = endpoint->Snapshot();
  EXPECT_EQ(snapshot.callback_count, 3);
  EXPECT_EQ(snapshot.callback_completed_count, 3);
  EXPECT_EQ(snapshot.callback_error_count, 2);
  EXPECT_EQ(snapshot.callback_inflight, 0);
  EXPECT_EQ(snapshot.callback_latency.count, 3);
  registry.Configure(Mode::Off);
}

}  // namespace
}  // namespace metrics
}  // namespace cyber
}  // namespace apollo

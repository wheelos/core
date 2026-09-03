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

#include <cmath>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tests/perf_test/benchmark_measurement_window.h"
#include "tests/perf_test/benchmark_process_common.h"
#include "tests/perf_test/benchmark_sequence_tracker.h"
#include "cyber/init.h"
#include "cyber/proto/unit_test.pb.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/transport.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

namespace {

struct SubscriberOptions {
  std::string worker_mode = "benchmark";
  CoverageMode coverage = CoverageMode::kInterProcess;
  MessageType message_type = MessageType::kProtobuf;
  std::string channel = "perf/default";
  std::string node_name = "benchmark_sub";
  std::string host_ip;
  std::string result_path = "/tmp/benchmark_sub_result.kv";
  std::string transport_mode_text;
  std::string latency_dump_path;
  std::string endpoint_ready_path;
  std::string warmup_ready_path;
  std::string measurement_start_path;
  int publishers = 1;
  int subscriber_index = 0;
  int duration_s = 3;
  int cooldown_wait_ms = 300;
  int readiness_timeout_s = 30;
  int cpu_interference_percent = 0;
  size_t latency_sample_cap = 5000000;
  uint64_t sequence_capacity = kDefaultSequenceCapacity;
  uint64_t start_ns = 0;
  int cpu = -1;
};

struct CpuInterferenceController {
  std::atomic<bool> run{false};
  std::vector<std::thread> workers;
};

void StartCpuInterference(CpuInterferenceController* controller,
                          int load_percent, int cpu) {
  if (controller == nullptr || load_percent <= 0) {
    return;
  }
  const int bounded = std::max(1, std::min(99, load_percent));
  controller->run.store(true, std::memory_order_release);
  controller->workers.emplace_back([controller, bounded, cpu]() {
    if (cpu >= 0) {
      (void)PinCurrentThreadToCpu(cpu);
    }
    const uint64_t cycle_ns = 1000000ULL;
    const uint64_t busy_ns =
        (cycle_ns * static_cast<uint64_t>(bounded)) / 100ULL;
    const uint64_t idle_ns = cycle_ns - busy_ns;
    while (controller->run.load(std::memory_order_acquire)) {
      const uint64_t start = MonotonicRawNowNs();
      while (MonotonicRawNowNs() - start < busy_ns) {
        asm volatile("" ::: "memory");
      }
      if (idle_ns > 0) {
        SleepNs(idle_ns);
      }
    }
  });
}

void StopCpuInterference(CpuInterferenceController* controller) {
  if (controller == nullptr) {
    return;
  }
  controller->run.store(false, std::memory_order_release);
  for (auto& worker : controller->workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  controller->workers.clear();
}

double PercentileFromSorted(const std::vector<uint64_t>& sorted,
                            double percentile) {
  if (sorted.empty()) {
    return 0.0;
  }
  if (percentile <= 0.0) {
    return static_cast<double>(sorted.front());
  }
  if (percentile >= 100.0) {
    return static_cast<double>(sorted.back());
  }
  const double rank =
      (percentile / 100.0) * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(rank));
  const size_t hi = static_cast<size_t>(std::ceil(rank));
  if (lo == hi) {
    return static_cast<double>(sorted[lo]);
  }
  const double weight = rank - static_cast<double>(lo);
  return (1.0 - weight) * static_cast<double>(sorted[lo]) +
         weight * static_cast<double>(sorted[hi]);
}

bool WriteLatencyDump(const std::string& path,
                      const std::vector<uint64_t>& samples,
                      uint64_t kept) {
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  const uint64_t count = std::min<uint64_t>(kept, samples.size());
  if (count > 0) {
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(count * sizeof(uint64_t)));
  }
  out.flush();
  return !out.fail();
}

void WriteErrorResult(const SubscriberOptions& options, const std::string& error) {
  WriteKvFile(options.result_path,
              {{"status", "error"},
               {"error", error},
               {"received_messages", "0"},
               {"received_bytes", "0"},
               {"sample_count", "0"},
               {"dropped_samples", "0"},
               {"latency_min_ns", "0"},
               {"latency_p50_ns", "0"},
               {"latency_p95_ns", "0"},
               {"latency_p99_ns", "0"},
               {"latency_p999_ns", "0"},
               {"latency_max_ns", "0"},
               {"cpu_delta_s", "0"},
               {"rss_kb_begin", "0"},
               {"rss_kb_end", "0"},
               {"voluntary_ctx_switches", "0"},
               {"involuntary_ctx_switches", "0"},
               {"zero_copy_borrowed_messages", "0"},
               {"zero_copy_copy_count", "0"}});
}

bool ParseSubscriberOptions(int argc, char** argv, SubscriberOptions* options,
                            std::string* error) {
  if (options == nullptr) {
    if (error != nullptr) {
      *error = "null options";
    }
    return false;
  }
  std::unordered_map<std::string, std::string> args;
  if (!ParseArgMap(argc, argv, &args, error)) {
    return false;
  }

  options->worker_mode = GetArgOr(args, "--worker_mode", options->worker_mode);
  options->channel = GetArgOr(args, "--channel", options->channel);
  options->node_name = GetArgOr(args, "--node_name", options->node_name);
  options->host_ip = GetArgOr(args, "--host_ip", options->host_ip);
  options->result_path = GetArgOr(args, "--result_path", options->result_path);
  options->transport_mode_text =
      GetArgOr(args, "--transport_mode", options->transport_mode_text);
  options->latency_dump_path =
      GetArgOr(args, "--latency_dump_path", options->result_path + ".lat");
  options->endpoint_ready_path =
      GetArgOr(args, "--endpoint_ready_path", options->endpoint_ready_path);
  options->warmup_ready_path =
      GetArgOr(args, "--warmup_ready_path", options->warmup_ready_path);
  options->measurement_start_path =
      GetArgOr(args, "--measurement_start_path",
               options->measurement_start_path);
  options->publishers =
      std::max(1, ParseIntOr(GetArgOr(args, "--publishers", "1"), 1));
  options->subscriber_index =
      std::max(0, ParseIntOr(GetArgOr(args, "--subscriber_index", "0"), 0));
  options->duration_s =
      std::max(1, ParseIntOr(GetArgOr(args, "--duration_s", "3"), 3));
  options->cooldown_wait_ms = std::max(
      0, ParseIntOr(GetArgOr(args, "--cooldown_wait_ms", "300"), 300));
  options->readiness_timeout_s = std::max(
      1, ParseIntOr(GetArgOr(args, "--readiness_timeout_s", "30"), 30));
  options->cpu_interference_percent = std::max(
      0, ParseIntOr(GetArgOr(args, "--cpu_interference_percent", "0"), 0));
  options->latency_sample_cap = static_cast<size_t>(std::max(
      1000, ParseIntOr(GetArgOr(args, "--latency_sample_cap", "5000000"), 5000000)));
  options->sequence_capacity = std::max<uint64_t>(
      1, ParseUInt64Or(GetArgOr(args, "--sequence_capacity",
                               std::to_string(kDefaultSequenceCapacity)),
                       kDefaultSequenceCapacity));
  options->start_ns = ParseUInt64Or(GetArgOr(args, "--start_ns", "0"), 0);
  options->cpu = ParseIntOr(GetArgOr(args, "--cpu", "-1"), -1);

  const std::string coverage_text =
      GetArgOr(args, "--coverage", ToString(options->coverage));
  if (!ParseCoverageMode(coverage_text, &options->coverage)) {
    if (error != nullptr) {
      *error = "invalid coverage: " + coverage_text;
    }
    return false;
  }
  const std::string message_type_text =
      GetArgOr(args, "--message_type", ToString(options->message_type));
  if (!ParseMessageType(message_type_text, &options->message_type)) {
    if (error != nullptr) {
      *error = "invalid message_type: " + message_type_text;
    }
    return false;
  }
  if (options->channel.empty()) {
    if (error != nullptr) {
      *error = "empty --channel";
    }
    return false;
  }
  if (options->result_path.empty()) {
    if (error != nullptr) {
      *error = "empty --result_path";
    }
    return false;
  }
  return true;
}

bool RunBenchmarkSubscriber(const SubscriberOptions& options, std::string* error) {
  if (options.message_type == MessageType::kPod &&
      options.coverage != CoverageMode::kInterProcess) {
    if (error != nullptr) {
      *error = "pod message_type currently requires --coverage=inter_process";
    }
    return false;
  }
  auto mode = ToTransportMode(options.coverage);
  if (!options.transport_mode_text.empty() &&
      !ParseOptionalMode(options.transport_mode_text, &mode)) {
    if (error != nullptr) {
      *error = "invalid transport_mode: " + options.transport_mode_text;
    }
    return false;
  }
  const int process_id = common::GlobalData::Instance()->ProcessId();
  const std::string host_ip = options.host_ip.empty()
                                  ? common::GlobalData::Instance()->HostIp()
                                  : options.host_ip;
  const auto attr = BuildRoleAttributes(
      options.channel, options.node_name, host_ip, process_id,
      static_cast<uint64_t>(options.subscriber_index + 1) * 131ULL);

  std::atomic<bool> active{true};
  std::atomic<uint64_t> received_messages{0};
  std::atomic<uint64_t> raw_received_messages{0};
  std::atomic<uint64_t> received_bytes{0};
  MeasurementWindowMetrics measured_metrics(
      static_cast<size_t>(options.publishers), options.latency_sample_cap);
  std::vector<PublisherSequenceTracker> trackers;
  trackers.reserve(static_cast<size_t>(options.publishers));
  for (int i = 0; i < options.publishers; ++i) {
    trackers.emplace_back(options.sequence_capacity);
  }
  std::vector<bool> warmup_received(static_cast<size_t>(options.publishers),
                                    false);
  std::mutex tracker_mutex;

  std::atomic<uint64_t> zero_copy_borrowed_messages{0};
  std::atomic<uint64_t> zero_copy_copy_count{0};

  std::function<bool(uint64_t, uint64_t, uint64_t, uint64_t)> update_tracker =
      [&](uint64_t sent_ns, uint64_t seq, uint64_t publisher_id,
          uint64_t payload_bytes) {
        SequenceObservation observation;
        {
          std::lock_guard<std::mutex> lock(tracker_mutex);
          if (!active.load(std::memory_order_acquire)) {
            return false;
          }
          if (seq >= kBenchmarkWarmupSeqBase) {
            if (publisher_id >= static_cast<uint64_t>(options.publishers)) {
              return false;
            }
            warmup_received[static_cast<size_t>(publisher_id)] = true;
            return false;
          }
          raw_received_messages.fetch_add(1, std::memory_order_relaxed);
          if (publisher_id >= static_cast<uint64_t>(options.publishers)) {
            return false;
          }
          observation =
              trackers[static_cast<size_t>(publisher_id)].Observe(seq);
          if (observation == SequenceObservation::kDuplicate ||
              observation == SequenceObservation::kOutOfWindow) {
            return false;
          }
          received_messages.fetch_add(1, std::memory_order_relaxed);
          received_bytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        }
        const uint64_t now_ns = MonotonicRawNowNs();
        const uint64_t latency_ns = now_ns >= sent_ns ? now_ns - sent_ns : 0;
        (void)measured_metrics.Record(static_cast<size_t>(publisher_id),
                                      payload_bytes, latency_ns);
        return true;
      };

  std::shared_ptr<transport::Receiver<apollo::cyber::proto::Chatter>>
      chatter_receiver;
  std::shared_ptr<transport::Receiver<transport::PodMessage>> pod_receiver;
  if (options.message_type == MessageType::kProtobuf) {
    chatter_receiver = transport::Transport::Instance()
                           ->CreateReceiver<apollo::cyber::proto::Chatter>(
                               attr,
                               [&](const std::shared_ptr<apollo::cyber::proto::Chatter>& msg,
                                   const transport::MessageInfo&,
                                   const apollo::cyber::proto::RoleAttributes&) {
                                 if (!active.load(std::memory_order_acquire) ||
                                     msg == nullptr) {
                                   return;
                                 }
                                 (void)update_tracker(
                                     msg->timestamp(), msg->seq(),
                                     msg->lidar_timestamp(),
                                     static_cast<uint64_t>(
                                         msg->content().size()));
                               },
                               mode);
  } else {
    pod_receiver = transport::Transport::Instance()
                       ->CreateReceiver<transport::PodMessage>(
                           attr,
                           [&](const std::shared_ptr<transport::PodMessage>& msg,
                               const transport::MessageInfo&,
                               const apollo::cyber::proto::RoleAttributes&) {
                             if (!active.load(std::memory_order_acquire) ||
                                 msg == nullptr) {
                               return;
                             }
                             const auto view = msg->View();
                             const bool unique = update_tracker(
                                 view.header.timestamp_ns, view.header.frame_id,
                                 view.header.width,
                                 static_cast<uint64_t>(view.payload_size));
                             if (unique && msg->is_borrowed()) {
                               zero_copy_borrowed_messages.fetch_add(
                                   1, std::memory_order_relaxed);
                             } else if (unique) {
                               zero_copy_copy_count.fetch_add(
                                   1, std::memory_order_relaxed);
                             }
                           },
                           mode);
  }
  if (chatter_receiver == nullptr && pod_receiver == nullptr) {
    if (error != nullptr) {
      *error = "failed to create receiver";
    }
    return false;
  }

  if (!options.endpoint_ready_path.empty() &&
      !WriteKvFile(options.endpoint_ready_path,
                   {{"endpoint_ready", "1"},
                    {"role", "subscriber"},
                    {"index", std::to_string(options.subscriber_index)}})) {
    if (error != nullptr) {
      *error = "failed to write subscriber endpoint readiness";
    }
    return false;
  }

  auto all_warmup_received = [&]() {
    std::lock_guard<std::mutex> lock(tracker_mutex);
    return std::all_of(warmup_received.begin(), warmup_received.end(),
                       [](bool received) { return received; });
  };
  bool warmup_confirmed = all_warmup_received();
  if (!options.measurement_start_path.empty()) {
    const uint64_t readiness_deadline =
        MonotonicRawNowNs() +
        static_cast<uint64_t>(options.readiness_timeout_s) * kOneSecondNs;
    while (!(warmup_confirmed = all_warmup_received())) {
      if (MonotonicRawNowNs() >= readiness_deadline) {
        if (error != nullptr) {
          std::ostringstream diagnostic;
          diagnostic << "subscriber warmup timeout: subscriber="
                     << options.subscriber_index << " missing_publishers=";
          std::lock_guard<std::mutex> lock(tracker_mutex);
          bool first = true;
          for (int i = 0; i < options.publishers; ++i) {
            if (!warmup_received[static_cast<size_t>(i)]) {
              diagnostic << (first ? "" : ",") << i;
              first = false;
            }
          }
          *error = diagnostic.str();
        }
        return false;
      }
      SleepNs(10 * kOneMillisecondNs);
    }
    if (!options.warmup_ready_path.empty() &&
        !WriteKvFile(options.warmup_ready_path,
                     {{"warmup_confirmed", "1"},
                      {"subscriber_index",
                       std::to_string(options.subscriber_index)}})) {
      if (error != nullptr) {
        *error = "failed to write subscriber warmup readiness";
      }
      return false;
    }
  }

  uint64_t start_ns = options.start_ns;
  if (!options.measurement_start_path.empty()) {
    const uint64_t start_deadline =
        MonotonicRawNowNs() +
        static_cast<uint64_t>(options.readiness_timeout_s) * kOneSecondNs;
    while (!ReadMeasurementStart(options.measurement_start_path, &start_ns)) {
      if (MonotonicRawNowNs() >= start_deadline) {
        if (error != nullptr) {
          *error = "subscriber timed out waiting for common measurement start";
        }
        return false;
      }
      SleepNs(10 * kOneMillisecondNs);
    }
  } else if (start_ns == 0) {
    start_ns = MonotonicRawNowNs() + 200 * kOneMillisecondNs;
  }
  if (start_ns <= MonotonicRawNowNs()) {
    if (error != nullptr) {
      *error = "subscriber received a non-future measurement start";
    }
    return false;
  }
  const uint64_t end_ns =
      start_ns + static_cast<uint64_t>(options.duration_s) * kOneSecondNs;
  SleepUntilNs(start_ns);
  const ResourceSnapshot begin = CaptureResourceSnapshot();
  measured_metrics.Start();
  CpuInterferenceController interference;
  StartCpuInterference(&interference, options.cpu_interference_percent,
                       options.cpu);
  active.store(true, std::memory_order_release);
  SleepUntilNs(end_ns);
  const MeasurementWindowSnapshot measured_snapshot =
      measured_metrics.StopAndSnapshot();
  const ResourceSnapshot measurement_end = CaptureResourceSnapshot();
  StopCpuInterference(&interference);

  SleepNs(static_cast<uint64_t>(options.cooldown_wait_ms) * kOneMillisecondNs);
  for (int i = 0; i < 20; ++i) {
    const uint64_t before = received_messages.load(std::memory_order_relaxed);
    SleepNs(50000000ULL);
    const uint64_t after = received_messages.load(std::memory_order_relaxed);
    if (before == after) {
      break;
    }
  }
  active.store(false, std::memory_order_release);
  const ResourceSnapshot cooldown_end = CaptureResourceSnapshot();
  if (chatter_receiver != nullptr) {
    chatter_receiver->Disable();
  }
  if (pod_receiver != nullptr) {
    pod_receiver->Disable();
  }

  if (!WriteLatencyDump(options.latency_dump_path,
                        measured_snapshot.latency_samples,
                        measured_snapshot.latency_samples.size())) {
    if (error != nullptr) {
      *error = "failed to write latency dump";
    }
    return false;
  }

  uint64_t latency_min = 0;
  uint64_t latency_p50 = 0;
  uint64_t latency_p95 = 0;
  uint64_t latency_p99 = 0;
  uint64_t latency_p999 = 0;
  uint64_t latency_max = 0;
  if (!measured_snapshot.latency_samples.empty()) {
    std::vector<uint64_t> sorted = measured_snapshot.latency_samples;
    std::sort(sorted.begin(), sorted.end());
    latency_min = sorted.front();
    latency_max = sorted.back();
    latency_p50 = static_cast<uint64_t>(PercentileFromSorted(sorted, 50.0));
    latency_p95 = static_cast<uint64_t>(PercentileFromSorted(sorted, 95.0));
    latency_p99 = static_cast<uint64_t>(PercentileFromSorted(sorted, 99.0));
    latency_p999 = static_cast<uint64_t>(PercentileFromSorted(sorted, 99.9));
  }

  const double cpu_delta =
      std::max(0.0, (measurement_end.cpu_user_s + measurement_end.cpu_sys_s) -
                        (begin.cpu_user_s + begin.cpu_sys_s));
  const uint64_t final_received =
      received_messages.load(std::memory_order_relaxed);
  const uint64_t final_received_bytes =
      received_bytes.load(std::memory_order_relaxed);
  const uint64_t cooldown_received_messages =
      final_received >= measured_snapshot.received_messages
          ? final_received - measured_snapshot.received_messages
          : 0;
  const uint64_t cooldown_received_bytes =
      final_received_bytes >= measured_snapshot.received_bytes
          ? final_received_bytes - measured_snapshot.received_bytes
          : 0;
  std::vector<std::pair<std::string, std::string>> kvs = {
      {"status", "ok"},
      {"worker_mode", "benchmark"},
      {"endpoint_ready", "1"},
      {"warmup_confirmed", warmup_confirmed ? "1" : "0"},
      {"measured_delivery_confirmed",
       measured_snapshot.DeliveryConfirmed() ? "1" : "0"},
      {"measurement_start_ns", std::to_string(start_ns)},
      {"measurement_duration_ns",
       std::to_string(measurement_end.wall_ns - begin.wall_ns)},
      {"received_messages",
       std::to_string(measured_snapshot.received_messages)},
      {"measured_received_messages",
       std::to_string(measured_snapshot.received_messages)},
      {"final_received_messages", std::to_string(final_received)},
      {"raw_received_messages",
       std::to_string(raw_received_messages.load(std::memory_order_relaxed))},
      {"received_bytes", std::to_string(measured_snapshot.received_bytes)},
      {"measured_received_bytes",
       std::to_string(measured_snapshot.received_bytes)},
      {"final_received_bytes", std::to_string(final_received_bytes)},
      {"received_at_measurement_end",
       std::to_string(measured_snapshot.received_messages)},
      {"received_bytes_at_measurement_end",
       std::to_string(measured_snapshot.received_bytes)},
      {"cooldown_received_messages",
       std::to_string(cooldown_received_messages)},
      {"cooldown_received_bytes",
       std::to_string(cooldown_received_bytes)},
      {"sample_count",
       std::to_string(measured_snapshot.latency_samples.size())},
      {"measured_sample_count",
       std::to_string(measured_snapshot.latency_samples.size())},
      {"dropped_samples", std::to_string(measured_snapshot.dropped_samples)},
      {"measured_dropped_samples",
       std::to_string(measured_snapshot.dropped_samples)},
      {"latency_min_ns", std::to_string(latency_min)},
      {"latency_p50_ns", std::to_string(latency_p50)},
      {"latency_p95_ns", std::to_string(latency_p95)},
      {"latency_p99_ns", std::to_string(latency_p99)},
      {"latency_p999_ns", std::to_string(latency_p999)},
      {"latency_max_ns", std::to_string(latency_max)},
      {"latency_dump_path", options.latency_dump_path},
      {"cpu_delta_s", std::to_string(cpu_delta)},
      {"rss_kb_begin", std::to_string(begin.rss_kb)},
      {"rss_kb_end", std::to_string(measurement_end.rss_kb)},
      {"rss_kb_cooldown_end", std::to_string(cooldown_end.rss_kb)},
      {"voluntary_ctx_switches",
       std::to_string(measurement_end.voluntary_ctx_switches -
                      begin.voluntary_ctx_switches)},
      {"involuntary_ctx_switches",
       std::to_string(measurement_end.involuntary_ctx_switches -
                      begin.involuntary_ctx_switches)},
      {"publishers", std::to_string(options.publishers)},
      {"message_type", ToString(options.message_type)},
      {"transport_mode", ToString(mode)},
      {"zero_copy_borrowed_messages",
       std::to_string(zero_copy_borrowed_messages.load(std::memory_order_relaxed))},
      {"zero_copy_copy_count",
       std::to_string(zero_copy_copy_count.load(std::memory_order_relaxed))},
  };
  {
    std::lock_guard<std::mutex> lock(tracker_mutex);
    for (int i = 0; i < options.publishers; ++i) {
      const auto& tracker = trackers[static_cast<size_t>(i)];
      kvs.emplace_back("tracker_" + std::to_string(i) + "_warmup_received",
                       warmup_received[static_cast<size_t>(i)] ? "1" : "0");
      kvs.emplace_back("tracker_" + std::to_string(i) + "_initialized",
                       tracker.initialized() ? "1" : "0");
      kvs.emplace_back("tracker_" + std::to_string(i) + "_expected_seq",
                       std::to_string(tracker.next_sequence()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_last_sequence",
                       std::to_string(tracker.last_sequence()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_received_unique",
                       std::to_string(tracker.received_unique()));
      kvs.emplace_back(
          "tracker_" + std::to_string(i) + "_final_received_unique",
          std::to_string(tracker.received_unique()));
      kvs.emplace_back(
          "tracker_" + std::to_string(i) + "_measured_received_unique",
          std::to_string(
              measured_snapshot.received_per_endpoint[static_cast<size_t>(i)]));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_gaps_observed",
                       std::to_string(tracker.gaps_observed()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_internal_loss",
                       std::to_string(
                           tracker.LossForSent(tracker.next_sequence())));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_max_consecutive_loss",
                       std::to_string(
                           tracker.MaxConsecutiveLossForSent(
                               tracker.next_sequence())));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_duplicates",
                       std::to_string(tracker.duplicates()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_final_duplicates",
                       std::to_string(tracker.duplicates()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_reordered",
                       std::to_string(tracker.reordered()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_final_reordered",
                       std::to_string(tracker.reordered()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_out_of_window",
                       std::to_string(tracker.out_of_window()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_sequence_capacity",
                       std::to_string(tracker.sequence_capacity()));
      kvs.emplace_back("tracker_" + std::to_string(i) + "_duplicate_or_reordered",
                       std::to_string(tracker.duplicates() +
                                      tracker.reordered()));
    }
  }
  const bool ok = WriteKvFile(options.result_path, kvs);
  if (!ok && error != nullptr) {
    *error = "failed to write benchmark subscriber result file";
  }
  return ok;
}

bool RunShmProbeSubscriber(const SubscriberOptions& options, std::string* error) {
  const int process_id = common::GlobalData::Instance()->ProcessId();
  const std::string host_ip = options.host_ip.empty()
                                  ? common::GlobalData::Instance()->HostIp()
                                  : options.host_ip;
  const auto attr = BuildRoleAttributes(options.channel, options.node_name, host_ip,
                                        process_id, 709);

  std::atomic<bool> active{true};
  std::atomic<uint64_t> received{0};
  std::atomic<uint64_t> borrowed{0};
  std::atomic<uint64_t> copy_count{0};
  std::atomic<uint64_t> sub_ptr_value{0};
  std::atomic<uint64_t> pub_ptr_from_header{0};

  auto receiver = transport::Transport::Instance()
                      ->CreateReceiver<transport::PodMessage>(
                          attr,
                          [&](const std::shared_ptr<transport::PodMessage>& msg,
                              const transport::MessageInfo&,
                              const apollo::cyber::proto::RoleAttributes&) {
                            if (!active.load(std::memory_order_acquire) ||
                                msg == nullptr) {
                              return;
                            }
                            received.fetch_add(1, std::memory_order_relaxed);
                            sub_ptr_value.store(
                                reinterpret_cast<uint64_t>(msg->data()),
                                std::memory_order_relaxed);
                            const auto* header = msg->header();
                            if (header != nullptr) {
                              pub_ptr_from_header.store(header->reserved[0],
                                                        std::memory_order_relaxed);
                              if (msg->is_borrowed()) {
                                borrowed.fetch_add(1, std::memory_order_relaxed);
                              } else {
                                copy_count.fetch_add(1, std::memory_order_relaxed);
                              }
                            } else {
                              copy_count.fetch_add(1, std::memory_order_relaxed);
                            }
                          },
                          apollo::cyber::proto::OptionalMode::SHM);
  if (receiver == nullptr) {
    if (error != nullptr) {
      *error = "failed to create pod receiver for shm probe";
    }
    return false;
  }

  const uint64_t start_ns =
      options.start_ns == 0 ? MonotonicRawNowNs() + 100 * kOneMillisecondNs
                            : options.start_ns;
  const uint64_t end_ns =
      start_ns + static_cast<uint64_t>(options.duration_s) * kOneSecondNs;
  SleepUntilNs(end_ns);
  for (int i = 0; i < 20; ++i) {
    if (received.load(std::memory_order_relaxed) > 0) {
      break;
    }
    SleepNs(50000000ULL);
  }
  active.store(false, std::memory_order_release);
  receiver->Disable();

  const uint64_t recv = received.load(std::memory_order_relaxed);
  const uint64_t borrowed_cnt = borrowed.load(std::memory_order_relaxed);
  const uint64_t copied_cnt = copy_count.load(std::memory_order_relaxed);
  const bool ok = WriteKvFile(
      options.result_path,
      {{"status", recv > 0 ? "ok" : "error"},
       {"worker_mode", "shm_probe"},
       {"probe_received", recv > 0 ? "1" : "0"},
       {"probe_received_messages", std::to_string(recv)},
       {"zero_copy_borrowed_messages", std::to_string(borrowed_cnt)},
       {"zero_copy_copy_count", std::to_string(copied_cnt)},
       {"probe_sub_ptr_value", std::to_string(sub_ptr_value.load())},
       {"probe_pub_ptr_from_header", std::to_string(pub_ptr_from_header.load())},
       {"received_messages", std::to_string(recv)},
       {"received_bytes", "0"},
       {"sample_count", "0"},
       {"dropped_samples", "0"},
       {"latency_min_ns", "0"},
       {"latency_p50_ns", "0"},
       {"latency_p95_ns", "0"},
       {"latency_p99_ns", "0"},
       {"latency_p999_ns", "0"},
       {"latency_max_ns", "0"},
       {"latency_dump_path", options.latency_dump_path},
       {"cpu_delta_s", "0"},
       {"rss_kb_begin", "0"},
       {"rss_kb_end", "0"},
       {"voluntary_ctx_switches", "0"},
       {"involuntary_ctx_switches", "0"},
       {"publishers", "1"},
       {"tracker_0_initialized", recv > 0 ? "1" : "0"},
       {"tracker_0_expected_seq", recv > 0 ? "1" : "0"},
       {"tracker_0_received_unique", std::to_string(recv)},
       {"tracker_0_internal_loss", "0"},
       {"tracker_0_max_consecutive_loss", "0"},
       {"tracker_0_duplicate_or_reordered", "0"}});
  if (!ok && error != nullptr) {
    *error = "failed to write shm probe subscriber result file";
  }
  return ok && recv > 0;
}

}  // namespace

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  setenv("GLOG_minloglevel", "3", 1);
  setenv("GLOG_v", "-1", 1);
  setenv("GLOG_logtostderr", "0", 1);
  setenv("GLOG_alsologtostderr", "0", 1);

  apollo::cyber::examples::perf_test::SubscriberOptions options;
  std::string parse_error;
  if (!apollo::cyber::examples::perf_test::ParseSubscriberOptions(
          argc, argv, &options, &parse_error)) {
    std::cerr << "benchmark_sub parse error: " << parse_error << std::endl;
    return 2;
  }

  if (options.cpu >= 0) {
    (void)apollo::cyber::examples::perf_test::PinCurrentThreadToCpu(options.cpu);
  }

  apollo::cyber::Init(argv[0]);
  apollo::cyber::transport::Transport::Instance();

  std::string run_error;
  bool ok = false;
  if (options.worker_mode == "shm_probe") {
    ok = apollo::cyber::examples::perf_test::RunShmProbeSubscriber(options,
                                                                    &run_error);
  } else {
    ok = apollo::cyber::examples::perf_test::RunBenchmarkSubscriber(options,
                                                                     &run_error);
  }
  if (!ok && !run_error.empty()) {
    apollo::cyber::examples::perf_test::WriteErrorResult(options, run_error);
  }
  apollo::cyber::Clear();
  const int exit_code = ok ? 0 : 1;
  if (!apollo::cyber::examples::perf_test::AppendKvFile(
          options.result_path,
          {{"shutdown_complete", "1"},
           {"planned_exit_code", std::to_string(exit_code)}})) {
    return 1;
  }
  return exit_code;
}

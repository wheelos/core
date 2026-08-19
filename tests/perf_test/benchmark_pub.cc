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

#include <cstdlib>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tests/perf_test/benchmark_process_common.h"
#include "cyber/init.h"
#include "cyber/proto/unit_test.pb.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/transport.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

namespace {

struct PublisherOptions {
  std::string worker_mode = "benchmark";
  CoverageMode coverage = CoverageMode::kInterProcess;
  MessageType message_type = MessageType::kProtobuf;
  std::string channel = "perf/default";
  std::string node_name = "benchmark_pub";
  std::string host_ip;
  std::string result_path = "/tmp/benchmark_pub_result.kv";
  std::string transport_mode_text;
  int publisher_index = 0;
  int frequency_hz = 1000;
  int payload_bytes = 1024;
  int duration_s = 3;
  int message_pool_depth = 1024;
  int cpu_interference_percent = 0;
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

void WriteErrorResult(const PublisherOptions& options, const std::string& error) {
  WriteKvFile(options.result_path,
              {{"status", "error"},
               {"error", error},
               {"sent_messages", "0"},
               {"send_failures", "0"},
               {"cpu_delta_s", "0"},
               {"rss_kb_begin", "0"},
               {"rss_kb_end", "0"},
               {"voluntary_ctx_switches", "0"},
               {"involuntary_ctx_switches", "0"}});
}

bool ParsePublisherOptions(int argc, char** argv, PublisherOptions* options,
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
  options->publisher_index =
      std::max(0, ParseIntOr(GetArgOr(args, "--publisher_index", "0"), 0));
  options->frequency_hz =
      std::max(0, ParseIntOr(GetArgOr(args, "--frequency_hz", "1000"), 1000));
  options->payload_bytes =
      std::max(1, ParseIntOr(GetArgOr(args, "--payload_bytes", "1024"), 1024));
  options->duration_s =
      std::max(1, ParseIntOr(GetArgOr(args, "--duration_s", "3"), 3));
  options->message_pool_depth = std::max(
      2, ParseIntOr(GetArgOr(args, "--message_pool_depth", "1024"), 1024));
  options->cpu_interference_percent = std::max(
      0, ParseIntOr(GetArgOr(args, "--cpu_interference_percent", "0"), 0));
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

bool RunBenchmarkPublisher(const PublisherOptions& options, std::string* error) {
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
      static_cast<uint64_t>(options.publisher_index + 1) * 97ULL);

  const std::string payload(static_cast<size_t>(options.payload_bytes), 'x');

  const uint64_t start_ns =
      options.start_ns == 0 ? MonotonicRawNowNs() + 200 * kOneMillisecondNs
                            : options.start_ns;
  const uint64_t end_ns =
      start_ns + static_cast<uint64_t>(options.duration_s) * kOneSecondNs;
  const uint64_t period_ns =
      options.frequency_hz > 0
          ? std::max<uint64_t>(1,
                               kOneSecondNs /
                                   static_cast<uint64_t>(options.frequency_hz))
          : 0;

  bool loan_supported = false;
  std::shared_ptr<transport::Transmitter<apollo::cyber::proto::Chatter>>
      chatter_transmitter;
  std::shared_ptr<transport::Transmitter<transport::PodMessage>>
      pod_transmitter;
  std::vector<std::shared_ptr<apollo::cyber::proto::Chatter>> pool;

  if (options.message_type == MessageType::kProtobuf) {
    chatter_transmitter = transport::Transport::Instance()
                              ->CreateTransmitter<apollo::cyber::proto::Chatter>(attr, mode);
    if (chatter_transmitter == nullptr) {
      if (error != nullptr) {
        *error = "failed to create chatter transmitter";
      }
      return false;
    }
    pool.reserve(static_cast<size_t>(options.message_pool_depth));
    for (int i = 0; i < options.message_pool_depth; ++i) {
      auto msg = std::make_shared<apollo::cyber::proto::Chatter>();
      msg->set_content(payload);
      msg->set_lidar_timestamp(static_cast<uint64_t>(options.publisher_index));
      msg->set_seq(0);
      msg->set_timestamp(0);
      pool.emplace_back(std::move(msg));
    }
  } else {
    pod_transmitter = transport::Transport::Instance()
                          ->CreateTransmitter<transport::PodMessage>(
                              attr, apollo::cyber::proto::OptionalMode::SHM);
    if (pod_transmitter == nullptr) {
      if (error != nullptr) {
        *error = "failed to create pod transmitter";
      }
      return false;
    }
    loan_supported = pod_transmitter->IsLoanSupported();
  }

  SleepUntilNs(start_ns);

  CpuInterferenceController interference;
  StartCpuInterference(&interference, options.cpu_interference_percent,
                       options.cpu);
  const ResourceSnapshot begin = CaptureResourceSnapshot();
  uint64_t sent_messages = 0;
  uint64_t send_failures = 0;
  uint64_t seq = 0;
  uint64_t next_send_ns = start_ns;

  if (options.message_type == MessageType::kProtobuf) {
    while (true) {
      const uint64_t now = MonotonicRawNowNs();
      if (now >= end_ns) {
        break;
      }
      if (period_ns > 0 && now < next_send_ns) {
        SleepUntilNs(next_send_ns);
        continue;
      }
      auto& msg = pool[static_cast<size_t>(seq % pool.size())];
      msg->set_seq(seq);
      msg->set_timestamp(now);
      msg->set_lidar_timestamp(static_cast<uint64_t>(options.publisher_index));
      if (chatter_transmitter->Transmit(msg)) {
        ++sent_messages;
      } else {
        ++send_failures;
      }
      ++seq;
      if (period_ns > 0) {
        next_send_ns += period_ns;
      }
    }
  } else {
    while (true) {
      const uint64_t now = MonotonicRawNowNs();
      if (now >= end_ns) {
        break;
      }
      if (period_ns > 0 && now < next_send_ns) {
        SleepUntilNs(next_send_ns);
        continue;
      }
      transport::PodChunkHeader header = transport::MakeImagePodChunkHeader(
          now, seq, static_cast<uint32_t>(options.publisher_index), 1, 1, 0,
          static_cast<uint32_t>(payload.size()));
      bool published = false;
      if (loan_supported) {
        transport::LoanedMessage<transport::PodMessage> loaned;
        const std::size_t required = transport::PodChunkTotalSize(payload.size());
        if (pod_transmitter->Loan(required, &loaned)) {
          const uint64_t ptr_value = reinterpret_cast<uint64_t>(loaned.data());
          header.reserved[0] = ptr_value;
          std::size_t written = 0;
          if (transport::BuildPodChunk(header, payload.data(), payload.size(),
                                       loaned.data(), loaned.capacity(), &written) &&
              loaned.set_size(written)) {
            published = pod_transmitter->Publish(std::move(loaned));
          }
        }
      }
      if (!published) {
        header.reserved[0] = 0;
        auto msg = std::make_shared<transport::PodMessage>(header, payload.data(),
                                                            payload.size());
        published = pod_transmitter->Transmit(msg);
      }
      if (published) {
        ++sent_messages;
      } else {
        ++send_failures;
      }
      ++seq;
      if (period_ns > 0) {
        next_send_ns += period_ns;
      }
    }
  }
  const ResourceSnapshot end = CaptureResourceSnapshot();
  StopCpuInterference(&interference);

  // on large RTPS payloads in short-lived benchmark workers.

  const double cpu_delta =
      std::max(0.0, (end.cpu_user_s + end.cpu_sys_s) -
                        (begin.cpu_user_s + begin.cpu_sys_s));
  const bool ok = WriteKvFile(
      options.result_path,
      {{"status", "ok"},
       {"worker_mode", "benchmark"},
       {"message_type", ToString(options.message_type)},
       {"transport_mode", ToString(mode)},
       {"sent_messages", std::to_string(sent_messages)},
       {"send_failures", std::to_string(send_failures)},
       {"loan_supported", loan_supported ? "1" : "0"},
       {"cpu_delta_s", std::to_string(cpu_delta)},
       {"rss_kb_begin", std::to_string(begin.rss_kb)},
       {"rss_kb_end", std::to_string(end.rss_kb)},
       {"voluntary_ctx_switches",
        std::to_string(end.voluntary_ctx_switches - begin.voluntary_ctx_switches)},
       {"involuntary_ctx_switches",
        std::to_string(end.involuntary_ctx_switches - begin.involuntary_ctx_switches)}});
  if (!ok && error != nullptr) {
    *error = "failed to write benchmark publisher result file";
  }
  return ok;
}

bool RunShmProbePublisher(const PublisherOptions& options, std::string* error) {
  const int process_id = common::GlobalData::Instance()->ProcessId();
  const std::string host_ip = options.host_ip.empty()
                                  ? common::GlobalData::Instance()->HostIp()
                                  : options.host_ip;
  const auto attr = BuildRoleAttributes(options.channel, options.node_name, host_ip,
                                        process_id, 601);
  auto transmitter = transport::Transport::Instance()
                         ->CreateTransmitter<transport::PodMessage>(
                             attr, apollo::cyber::proto::OptionalMode::SHM);
  if (transmitter == nullptr) {
    if (error != nullptr) {
      *error = "failed to create pod transmitter for shm probe";
    }
    return false;
  }

  const uint64_t start_ns =
      options.start_ns == 0 ? MonotonicRawNowNs() + 100 * kOneMillisecondNs
                            : options.start_ns;
  const uint64_t end_ns =
      start_ns + static_cast<uint64_t>(options.duration_s) * kOneSecondNs;
  SleepUntilNs(start_ns);

  const std::string payload(static_cast<size_t>(options.payload_bytes), 'z');
  bool published = false;
  uint64_t pub_ptr_value = 0;
  const bool loan_supported = transmitter->IsLoanSupported();
  std::string publish_mode = "fallback_transmit";
  uint64_t sent_count = 0;

  for (uint64_t frame = 0; MonotonicRawNowNs() < end_ns; ++frame) {
    transport::PodChunkHeader header = transport::MakeImagePodChunkHeader(
        MonotonicRawNowNs(), frame, 1, 1, 1, 0,
        static_cast<uint32_t>(payload.size()));
    bool current_published = false;
    if (loan_supported) {
      transport::LoanedMessage<transport::PodMessage> loaned;
      const std::size_t required = transport::PodChunkTotalSize(payload.size());
      if (transmitter->Loan(required, &loaned)) {
        pub_ptr_value = reinterpret_cast<uint64_t>(loaned.data());
        header.reserved[0] = pub_ptr_value;
        std::size_t written = 0;
        if (transport::BuildPodChunk(header, payload.data(), payload.size(),
                                     loaned.data(), loaned.capacity(), &written) &&
            loaned.set_size(written)) {
          current_published = transmitter->Publish(std::move(loaned));
          publish_mode = "loan_publish";
        }
      }
    }

    if (!current_published) {
      header.reserved[0] = 0;
      auto msg = std::make_shared<transport::PodMessage>(header, payload.data(),
                                                          payload.size());
      current_published = transmitter->Transmit(msg);
    }
    if (current_published) {
      published = true;
      ++sent_count;
    }
    SleepNs(100 * kOneMillisecondNs);
  }

  const bool ok = WriteKvFile(
      options.result_path,
      {{"status", published ? "ok" : "error"},
       {"worker_mode", "shm_probe"},
       {"probe_published", published ? "1" : "0"},
       {"probe_loan_supported", loan_supported ? "1" : "0"},
       {"probe_pub_ptr_value", std::to_string(pub_ptr_value)},
       {"probe_publish_mode", publish_mode},
       {"sent_messages", std::to_string(sent_count)},
       {"send_failures", published ? "0" : "1"},
       {"cpu_delta_s", "0"},
       {"rss_kb_begin", "0"},
       {"rss_kb_end", "0"},
       {"voluntary_ctx_switches", "0"},
       {"involuntary_ctx_switches", "0"}});
  if (!ok && error != nullptr) {
    *error = "failed to write shm probe publisher result file";
  }
  return ok && published;
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

  apollo::cyber::examples::perf_test::PublisherOptions options;
  std::string parse_error;
  if (!apollo::cyber::examples::perf_test::ParsePublisherOptions(
          argc, argv, &options, &parse_error)) {
    std::cerr << "benchmark_pub parse error: " << parse_error << std::endl;
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
    ok = apollo::cyber::examples::perf_test::RunShmProbePublisher(options,
                                                                   &run_error);
  } else {
    ok = apollo::cyber::examples::perf_test::RunBenchmarkPublisher(options,
                                                                    &run_error);
  }
  if (!ok && !run_error.empty()) {
    apollo::cyber::examples::perf_test::WriteErrorResult(options, run_error);
  }
  apollo::cyber::Clear();
  return ok ? 0 : 1;
}

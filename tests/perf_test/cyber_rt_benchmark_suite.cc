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

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/init.h"
#include "cyber/proto/unit_test.pb.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/qos/qos_profile_conf.h"
#include "cyber/transport/iceoryx_chunk.h"
#include "cyber/transport/shm/profile.h"
#include "cyber/transport/transport.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

namespace {

constexpr uint64_t kOneSecondNs = 1000000000ULL;
constexpr uint64_t kOneMillisecondNs = 1000000ULL;
constexpr uint64_t kOneMegabyte = 1024ULL * 1024ULL;

uint64_t MonotonicRawNowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * kOneSecondNs +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t duration_ns) {
  if (duration_ns == 0) {
    return;
  }
  timespec req;
  req.tv_sec = static_cast<time_t>(duration_ns / kOneSecondNs);
  req.tv_nsec = static_cast<long>(duration_ns % kOneSecondNs);
  while (nanosleep(&req, &req) != 0 && errno == EINTR) {
  }
}

void SleepUntilNs(uint64_t target_ns) {
  while (true) {
    const uint64_t now = MonotonicRawNowNs();
    if (now >= target_ns) {
      return;
    }
    const uint64_t remain = target_ns - now;
    if (remain > 50000ULL) {  // >50us
      SleepNs(remain - 20000ULL);
    } else {
      std::this_thread::yield();
    }
  }
}

std::string JsonEscape(const std::string& input) {
  std::ostringstream oss;
  for (const unsigned char c : input) {
    switch (c) {
      case '\"':
        oss << "\\\"";
        break;
      case '\\':
        oss << "\\\\";
        break;
      case '\b':
        oss << "\\b";
        break;
      case '\f':
        oss << "\\f";
        break;
      case '\n':
        oss << "\\n";
        break;
      case '\r':
        oss << "\\r";
        break;
      case '\t':
        oss << "\\t";
        break;
      default:
        if (c < 0x20) {
          oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(c) << std::dec << std::setfill(' ');
        } else {
          oss << static_cast<char>(c);
        }
        break;
    }
  }
  return oss.str();
}

int ParseIntOr(const std::string& value, int fallback) {
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

bool ParseBoolOr(const std::string& value, bool fallback) {
  if (value == "1" || value == "true" || value == "TRUE" ||
      value == "True") {
    return true;
  }
  if (value == "0" || value == "false" || value == "FALSE" ||
      value == "False") {
    return false;
  }
  return fallback;
}

std::vector<int> ParseCpuSet(const std::string& value) {
  std::vector<int> cpus;
  std::stringstream ss(value);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) {
      continue;
    }
    const int cpu = ParseIntOr(token, -1);
    if (cpu >= 0) {
      cpus.push_back(cpu);
    }
  }
  if (cpus.empty()) {
    cpus.push_back(0);
  }
  return cpus;
}

bool PinCurrentThreadToCpu(int cpu) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
}

long ReadVmRssKb() {
  std::ifstream in("/proc/self/status");
  if (!in.is_open()) {
    return 0;
  }
  std::string line;
  while (std::getline(in, line)) {
    static constexpr char kPrefix[] = "VmRSS:";
    if (line.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0) {
      std::stringstream ss(line.substr(sizeof(kPrefix) - 1));
      long rss_kb = 0;
      ss >> rss_kb;
      return rss_kb;
    }
  }
  return 0;
}

struct ResourceSnapshot {
  uint64_t wall_ns = 0;
  double cpu_user_s = 0.0;
  double cpu_sys_s = 0.0;
  long rss_kb = 0;
  long voluntary_ctx_switches = 0;
  long involuntary_ctx_switches = 0;
};

ResourceSnapshot CaptureResourceSnapshot() {
  ResourceSnapshot snapshot;
  snapshot.wall_ns = MonotonicRawNowNs();
  rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    snapshot.cpu_user_s =
        static_cast<double>(usage.ru_utime.tv_sec) +
        static_cast<double>(usage.ru_utime.tv_usec) / 1e6;
    snapshot.cpu_sys_s =
        static_cast<double>(usage.ru_stime.tv_sec) +
        static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
    snapshot.voluntary_ctx_switches = usage.ru_nvcsw;
    snapshot.involuntary_ctx_switches = usage.ru_nivcsw;
  }
  snapshot.rss_kb = ReadVmRssKb();
  return snapshot;
}

double SafeDiv(double a, double b) {
  if (std::abs(b) < std::numeric_limits<double>::epsilon()) {
    return 0.0;
  }
  return a / b;
}

uint64_t ParseUInt64Or(const std::string& value, uint64_t fallback) {
  if (value.empty()) {
    return fallback;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    return fallback;
  }
  return static_cast<uint64_t>(parsed);
}

double ParseDoubleOr(const std::string& value, double fallback) {
  if (value.empty()) {
    return fallback;
  }
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(value.c_str(), &end);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    return fallback;
  }
  return parsed;
}

std::string JoinCpuSet(const std::vector<int>& cpus) {
  std::ostringstream oss;
  for (size_t i = 0; i < cpus.size(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << cpus[i];
  }
  return oss.str();
}

std::string DirName(const std::string& path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return ".";
  }
  if (pos == 0) {
    return "/";
  }
  return path.substr(0, pos);
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty() || dir == ".") {
    return name;
  }
  if (dir.back() == '/') {
    return dir + name;
  }
  return dir + "/" + name;
}

bool ReadKvFile(const std::string& path,
                std::unordered_map<std::string, std::string>* kv) {
  if (kv == nullptr) {
    return false;
  }
  kv->clear();
  std::ifstream in(path);
  if (!in.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    (*kv)[line.substr(0, pos)] = line.substr(pos + 1);
  }
  return true;
}

bool ReadLatencyDump(const std::string& path, std::vector<uint64_t>* samples) {
  if (samples == nullptr) {
    return false;
  }
  samples->clear();
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) {
    return true;
  }
  in.seekg(0, std::ios::beg);
  const size_t count = static_cast<size_t>(size / sizeof(uint64_t));
  samples->resize(count, 0);
  in.read(reinterpret_cast<char*>(samples->data()),
          static_cast<std::streamsize>(count * sizeof(uint64_t)));
  return !in.fail();
}

bool FileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

void RemoveFileIfExists(const std::string& path) {
  if (!path.empty()) {
    (void)unlink(path.c_str());
  }
}

struct ChildProcess {
  pid_t pid = -1;
  std::string role;
  std::string result_path;
};

bool SpawnProcess(const std::vector<std::string>& args, ChildProcess* child,
                  std::string* error) {
  if (child == nullptr || args.empty()) {
    if (error != nullptr) {
      *error = "invalid spawn parameters";
    }
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    if (error != nullptr) {
      *error = "fork failed";
    }
    return false;
  }
  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execv(args[0].c_str(), argv.data());
    _Exit(127);
  }
  child->pid = pid;
  return true;
}

bool WaitForChildren(std::vector<ChildProcess>* children, int timeout_s,
                     std::string* error) {
  if (children == nullptr) {
    if (error != nullptr) {
      *error = "null children list";
    }
    return false;
  }
  std::unordered_map<pid_t, size_t> pid_to_index;
  for (size_t i = 0; i < children->size(); ++i) {
    pid_to_index[(*children)[i].pid] = i;
  }
  const uint64_t deadline = MonotonicRawNowNs() +
                            static_cast<uint64_t>(std::max(1, timeout_s)) *
                                kOneSecondNs;
  std::vector<bool> done(children->size(), false);
  size_t done_count = 0;
  while (done_count < children->size()) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid == 0) {
      if (MonotonicRawNowNs() >= deadline) {
        for (const auto& child : *children) {
          if (child.pid > 0) {
            (void)kill(child.pid, SIGKILL);
          }
        }
        for (const auto& child : *children) {
          if (child.pid > 0) {
            (void)waitpid(child.pid, nullptr, 0);
          }
        }
        if (error != nullptr) {
          *error = "worker process timeout";
        }
        return false;
      }
      SleepNs(10000000ULL);
      continue;
    }
    if (pid < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error != nullptr) {
        *error = "waitpid failed";
      }
      return false;
    }
    const auto it = pid_to_index.find(pid);
    if (it == pid_to_index.end()) {
      continue;
    }
    const size_t idx = it->second;
    if (!done[idx]) {
      done[idx] = true;
      ++done_count;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      for (size_t i = 0; i < children->size(); ++i) {
        if (!done[i] && (*children)[i].pid > 0) {
          (void)kill((*children)[i].pid, SIGKILL);
        }
      }
      for (size_t i = 0; i < children->size(); ++i) {
        if (!done[i] && (*children)[i].pid > 0) {
          (void)waitpid((*children)[i].pid, nullptr, 0);
          done[i] = true;
        }
      }
      if (error != nullptr) {
        *error = "worker exited abnormally";
      }
      return false;
    }
  }
  return true;
}

std::string CreateCaseTempDir(uint64_t case_id) {
  std::string tmpl = "/tmp/cyber_rt_perf_case_" + std::to_string(case_id) +
                     "_XXXXXX";
  std::vector<char> mutable_path(tmpl.begin(), tmpl.end());
  mutable_path.push_back('\0');
  char* path = mkdtemp(mutable_path.data());
  if (path == nullptr) {
    return "";
  }
  return std::string(path);
}

enum class CoverageMode {
  kIntraProcess,
  kInterProcess,
  kInterHost,
};

enum class TopologyMode {
  kOnePubOneSub,
  kNPubOneSub,
  kOnePubMSub,
};

enum class ScenarioKind {
  kShmZeroCopyProbe,
  kZeroCopyVsProtobuf,
  kFloodMode,
  kPayloadSweep,
  kFanoutScaling,
  kFrequencySweep,
  kBandwidthSweep,
  kSubscriberScaling,
  kPublisherScaling,
  kCpuInterference,
  kLongRun,
};

std::string ToString(CoverageMode mode) {
  switch (mode) {
    case CoverageMode::kIntraProcess:
      return "intra_process";
    case CoverageMode::kInterProcess:
      return "inter_process";
    case CoverageMode::kInterHost:
      return "inter_host";
  }
  return "unknown";
}

std::string ToString(TopologyMode mode) {
  switch (mode) {
    case TopologyMode::kOnePubOneSub:
      return "1_pub_1_sub";
    case TopologyMode::kNPubOneSub:
      return "n_pub_1_sub";
    case TopologyMode::kOnePubMSub:
      return "1_pub_m_sub";
  }
  return "unknown";
}

std::string ToString(ScenarioKind kind) {
  switch (kind) {
    case ScenarioKind::kShmZeroCopyProbe:
      return "shm_zero_copy_probe";
    case ScenarioKind::kZeroCopyVsProtobuf:
      return "zero_copy_vs_protobuf";
    case ScenarioKind::kFloodMode:
      return "flood_mode";
    case ScenarioKind::kPayloadSweep:
      return "payload_sweep";
    case ScenarioKind::kFanoutScaling:
      return "fanout_scaling";
    case ScenarioKind::kFrequencySweep:
      return "frequency_sweep";
    case ScenarioKind::kBandwidthSweep:
      return "bandwidth_sweep";
    case ScenarioKind::kSubscriberScaling:
      return "subscriber_scaling";
    case ScenarioKind::kPublisherScaling:
      return "publisher_scaling";
    case ScenarioKind::kCpuInterference:
      return "cpu_interference";
    case ScenarioKind::kLongRun:
      return "long_run";
  }
  return "unknown";
}

enum class MessageType {
  kProtobuf,
  kPod,
};

std::string ToString(MessageType type) {
  switch (type) {
    case MessageType::kProtobuf:
      return "protobuf";
    case MessageType::kPod:
      return "pod";
  }
  return "protobuf";
}

std::string ToString(apollo::cyber::proto::OptionalMode mode) {
  switch (mode) {
    case apollo::cyber::proto::OptionalMode::INTRA:
      return "intra";
    case apollo::cyber::proto::OptionalMode::SHM:
      return "shm";
    case apollo::cyber::proto::OptionalMode::RTPS:
      return "rtps";
    case apollo::cyber::proto::OptionalMode::HYBRID:
      return "hybrid";
    case apollo::cyber::proto::OptionalMode::ICEORYX:
      return "iceoryx";
  }
  return "hybrid";
}


apollo::cyber::proto::OptionalMode ToTransportMode(CoverageMode coverage) {
  switch (coverage) {
    case CoverageMode::kIntraProcess:
      return apollo::cyber::proto::OptionalMode::INTRA;
    case CoverageMode::kInterProcess:
      return apollo::cyber::proto::OptionalMode::SHM;
    case CoverageMode::kInterHost:
      return apollo::cyber::proto::OptionalMode::RTPS;
  }
  return apollo::cyber::proto::OptionalMode::HYBRID;
}

struct BenchmarkOptions {
  std::string output_path = "cyber_rt_benchmark_results.json";
  std::vector<int> cpu_set = {0};
  std::string benchmark_pub_binary;
  std::string benchmark_sub_binary;
  int process_case_timeout_s = 180;
  bool use_real_inter_process = true;

  int startup_wait_ms = 600;
  int cooldown_wait_ms = 500;

  int frequency_payload_bytes = 1024;
  int frequency_start_hz = 100;
  int frequency_end_hz = 5000;
  int frequency_step_hz = 100;
  int frequency_case_duration_s = 3;

  int bandwidth_frequency_hz = 30;
  int bandwidth_min_mb = 1;
  int bandwidth_max_mb = 10;
  int bandwidth_step_mb = 1;
  int bandwidth_case_duration_s = 3;

  int scaling_frequency_hz = 1000;
  int scaling_payload_bytes = 1024;
  int scaling_case_duration_s = 3;
  int max_subscribers = 32;
  int max_publishers = 32;

  int cpu_interference_frequency_hz = 1000;
  int cpu_interference_payload_bytes = 1024;
  int cpu_interference_duration_s = 5;
  std::vector<int> cpu_interference_levels = {50, 75, 90};

  bool enable_long_run = false;
  int long_run_seconds = 24 * 60 * 60;
  int long_run_frequency_hz = 200;
  int long_run_payload_bytes = 1024;

  int message_pool_depth = 1024;
  int message_pool_budget_mb = 256;
  size_t latency_sample_cap = 5000000;

  bool run_frequency_sweep = true;
  bool run_bandwidth_sweep = true;
  bool run_subscriber_scaling = true;
  bool run_publisher_scaling = true;
  bool run_cpu_interference = true;
  bool run_flood_mode_comparison = false;
  bool run_payload_sweep_comparison = false;
  bool run_fanout_scaling_comparison = false;

  int flood_duration_s = 5;
  int flood_msg_payload_bytes = 4096;
  int flood_bw_payload_mb = 7;
  int payload_sweep_frequency_hz = 100;
  int payload_sweep_duration_s = 5;
  std::vector<int> payload_sweep_sizes_mb = {1, 4, 7};
  int fanout_frequency_hz = 220;
  int fanout_payload_mb = 7;
  int fanout_duration_s = 5;
  std::vector<int> fanout_subscribers = {1, 3, 5, 8};

  bool quick_mode = false;
};

struct BenchmarkCaseConfig {
  ScenarioKind scenario = ScenarioKind::kFrequencySweep;
  CoverageMode coverage = CoverageMode::kIntraProcess;
  MessageType message_type = MessageType::kProtobuf;
  TopologyMode topology = TopologyMode::kOnePubOneSub;
  int publishers = 1;
  int subscribers = 1;
  int frequency_hz = 1000;
  int payload_bytes = 1024;
  int duration_s = 3;
  int cpu_interference_percent = 0;
};

std::string WorkerTransportModeForCase(const BenchmarkCaseConfig& config) {
  if (config.message_type == MessageType::kPod &&
      config.coverage == CoverageMode::kInterProcess) {
    return "iceoryx";
  }
  return ToString(ToTransportMode(config.coverage));
}

struct LatencyStats {
  uint64_t min_ns = 0;
  uint64_t p50_ns = 0;
  uint64_t p95_ns = 0;
  uint64_t p99_ns = 0;
  uint64_t p999_ns = 0;
  uint64_t max_ns = 0;
  uint64_t sample_count = 0;
  uint64_t dropped_samples = 0;
};

struct ThroughputStats {
  double messages_per_s = 0.0;
  double mb_per_s = 0.0;
  uint64_t received_messages = 0;
  uint64_t received_bytes = 0;
  uint64_t sent_messages = 0;
  uint64_t send_failures = 0;
};

struct ReliabilityStats {
  double loss_rate = 0.0;
  uint64_t total_loss = 0;
  uint64_t max_consecutive_loss = 0;
  uint64_t duplicate_or_reordered = 0;
};

struct ResourceUsageStats {
  double cpu_cost_us_per_message = 0.0;
  double cpu_utilization_percent = 0.0;
  long rss_kb_begin = 0;
  long rss_kb_end = 0;
  long rss_kb_peak_observed = 0;
  long context_switches = 0;
  long voluntary_context_switches = 0;
  long involuntary_context_switches = 0;
};

struct BenchmarkCaseResult {
  BenchmarkCaseConfig config;
  bool success = false;
  std::string error_message;
  std::string notes;

  LatencyStats latency;
  ThroughputStats throughput;
  ReliabilityStats reliability;
  ResourceUsageStats resource;

  bool shm_profile_recorded = false;
  bool shm_loan_supported = false;

  uint64_t wall_time_ns = 0;
  int message_pool_depth = 0;
};

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

struct SequenceTracker {
  bool initialized = false;
  uint64_t expected_seq = 0;
  uint64_t received_unique = 0;
  uint64_t internal_loss = 0;
  uint64_t max_consecutive_loss = 0;
  uint64_t duplicate_or_reordered = 0;
};

struct RuntimeCounters {
  std::atomic<uint64_t> received_messages{0};
  std::atomic<uint64_t> received_bytes{0};
  std::atomic<uint64_t> sample_count{0};
  std::atomic<uint64_t> dropped_samples{0};

  std::vector<uint64_t> latency_samples;
  std::vector<std::vector<SequenceTracker>> sequence_trackers;
  std::vector<std::unique_ptr<std::mutex>> sequence_locks;
};

struct PublisherMessagePool {
  std::vector<std::shared_ptr<apollo::cyber::proto::Chatter>> messages;
};

struct CpuInterferenceController {
  std::atomic<bool> run{false};
  std::vector<std::thread> workers;
};

std::string MakeChannelName(const BenchmarkCaseConfig& config, uint64_t suffix) {
  std::ostringstream oss;
  oss << "perf/" << ToString(config.scenario) << "/" << ToString(config.coverage)
      << "/" << ToString(config.message_type) << "/" << ToString(config.topology)
      << "/p" << config.publishers << "_s" << config.subscribers << "_f"
      << config.frequency_hz << "_b"
      << config.payload_bytes << "_x" << config.cpu_interference_percent << "_"
      << suffix;
  return oss.str();
}

apollo::cyber::proto::RoleAttributes BuildRoleAttributes(
    const std::string& channel_name, const std::string& node_name,
    const std::string& host_ip, int process_id, uint64_t unique_seed) {
  apollo::cyber::proto::RoleAttributes attr;
  attr.set_channel_name(channel_name);
  attr.set_channel_id(common::GlobalData::RegisterChannel(channel_name));
  attr.set_node_name(node_name);
  attr.set_node_id(common::GlobalData::RegisterNode(node_name));
  attr.set_host_name(common::GlobalData::Instance()->HostName());
  attr.set_host_ip(host_ip);
  attr.set_process_id(process_id);
  attr.set_id(common::GlobalData::GenerateHashId(
      node_name + "_" + channel_name + "_" + std::to_string(unique_seed)));
  attr.mutable_qos_profile()->CopyFrom(
      transport::QosProfileConf::QOS_PROFILE_DEFAULT);
  attr.mutable_qos_profile()->set_depth(4096);
  return attr;
}

int ComputeMessagePoolDepth(const BenchmarkOptions& options,
                            const BenchmarkCaseConfig& config) {
  const int max_depth = std::max(1, options.message_pool_depth);
  if (config.payload_bytes <= 0) {
    return max_depth;
  }
  const uint64_t budget_bytes =
      static_cast<uint64_t>(std::max(1, options.message_pool_budget_mb)) *
      kOneMegabyte;
  const uint64_t publishers = static_cast<uint64_t>(std::max(1, config.publishers));
  const uint64_t budget_per_publisher = std::max<uint64_t>(1, budget_bytes / publishers);
  uint64_t depth = budget_per_publisher / static_cast<uint64_t>(config.payload_bytes);
  if (depth == 0) {
    depth = 1;
  }
  depth = std::max<uint64_t>(depth, 2);
  depth = std::min<uint64_t>(depth, static_cast<uint64_t>(max_depth));
  return static_cast<int>(depth);
}

bool ParseOptions(int argc, char** argv, BenchmarkOptions* options,
                  std::string* error) {
  if (options == nullptr) {
    if (error != nullptr) {
      *error = "null options";
    }
    return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      return false;
    }
    if (arg == "--quick") {
      options->quick_mode = true;
      continue;
    }
    const auto pos = arg.find('=');
    if (pos == std::string::npos) {
      if (error != nullptr) {
        *error = "invalid argument format: " + arg;
      }
      return false;
    }
    const std::string key = arg.substr(0, pos);
    const std::string value = arg.substr(pos + 1);
    if (key == "--output_json") {
      options->output_path = value;
    } else if (key == "--cpu_set") {
      options->cpu_set = ParseCpuSet(value);
    } else if (key == "--benchmark_pub_bin") {
      options->benchmark_pub_binary = value;
    } else if (key == "--benchmark_sub_bin") {
      options->benchmark_sub_binary = value;
    } else if (key == "--process_case_timeout_s") {
      options->process_case_timeout_s =
          std::max(10, ParseIntOr(value, options->process_case_timeout_s));
    } else if (key == "--use_real_inter_process") {
      options->use_real_inter_process =
          ParseBoolOr(value, options->use_real_inter_process);
    } else if (key == "--startup_wait_ms") {
      options->startup_wait_ms = std::max(0, ParseIntOr(value, options->startup_wait_ms));
    } else if (key == "--cooldown_wait_ms") {
      options->cooldown_wait_ms =
          std::max(0, ParseIntOr(value, options->cooldown_wait_ms));
    } else if (key == "--frequency_start_hz") {
      options->frequency_start_hz = std::max(1, ParseIntOr(value, options->frequency_start_hz));
    } else if (key == "--frequency_end_hz") {
      options->frequency_end_hz = std::max(1, ParseIntOr(value, options->frequency_end_hz));
    } else if (key == "--frequency_step_hz") {
      options->frequency_step_hz = std::max(1, ParseIntOr(value, options->frequency_step_hz));
    } else if (key == "--frequency_case_duration_s") {
      options->frequency_case_duration_s =
          std::max(1, ParseIntOr(value, options->frequency_case_duration_s));
    } else if (key == "--frequency_payload_bytes") {
      options->frequency_payload_bytes =
          std::max(1, ParseIntOr(value, options->frequency_payload_bytes));
    } else if (key == "--bandwidth_frequency_hz") {
      options->bandwidth_frequency_hz =
          std::max(1, ParseIntOr(value, options->bandwidth_frequency_hz));
    } else if (key == "--bandwidth_min_mb") {
      options->bandwidth_min_mb = std::max(1, ParseIntOr(value, options->bandwidth_min_mb));
    } else if (key == "--bandwidth_max_mb") {
      options->bandwidth_max_mb = std::max(1, ParseIntOr(value, options->bandwidth_max_mb));
    } else if (key == "--bandwidth_step_mb") {
      options->bandwidth_step_mb = std::max(1, ParseIntOr(value, options->bandwidth_step_mb));
    } else if (key == "--bandwidth_case_duration_s") {
      options->bandwidth_case_duration_s =
          std::max(1, ParseIntOr(value, options->bandwidth_case_duration_s));
    } else if (key == "--scaling_frequency_hz") {
      options->scaling_frequency_hz =
          std::max(1, ParseIntOr(value, options->scaling_frequency_hz));
    } else if (key == "--scaling_payload_bytes") {
      options->scaling_payload_bytes =
          std::max(1, ParseIntOr(value, options->scaling_payload_bytes));
    } else if (key == "--scaling_case_duration_s") {
      options->scaling_case_duration_s =
          std::max(1, ParseIntOr(value, options->scaling_case_duration_s));
    } else if (key == "--max_subscribers") {
      options->max_subscribers = std::max(1, ParseIntOr(value, options->max_subscribers));
    } else if (key == "--max_publishers") {
      options->max_publishers = std::max(1, ParseIntOr(value, options->max_publishers));
    } else if (key == "--cpu_interference_duration_s") {
      options->cpu_interference_duration_s =
          std::max(1, ParseIntOr(value, options->cpu_interference_duration_s));
    } else if (key == "--cpu_interference_frequency_hz") {
      options->cpu_interference_frequency_hz =
          std::max(1, ParseIntOr(value, options->cpu_interference_frequency_hz));
    } else if (key == "--cpu_interference_payload_bytes") {
      options->cpu_interference_payload_bytes =
          std::max(1, ParseIntOr(value, options->cpu_interference_payload_bytes));
    } else if (key == "--enable_long_run") {
      options->enable_long_run = ParseBoolOr(value, options->enable_long_run);
    } else if (key == "--long_run_seconds") {
      options->long_run_seconds = std::max(1, ParseIntOr(value, options->long_run_seconds));
    } else if (key == "--long_run_frequency_hz") {
      options->long_run_frequency_hz =
          std::max(1, ParseIntOr(value, options->long_run_frequency_hz));
    } else if (key == "--long_run_payload_bytes") {
      options->long_run_payload_bytes =
          std::max(1, ParseIntOr(value, options->long_run_payload_bytes));
    } else if (key == "--message_pool_depth") {
      options->message_pool_depth = std::max(1, ParseIntOr(value, options->message_pool_depth));
    } else if (key == "--message_pool_budget_mb") {
      options->message_pool_budget_mb =
          std::max(1, ParseIntOr(value, options->message_pool_budget_mb));
    } else if (key == "--latency_sample_cap") {
      options->latency_sample_cap = static_cast<size_t>(
          std::max(1000, ParseIntOr(value, static_cast<int>(options->latency_sample_cap))));
    } else if (key == "--run_frequency_sweep") {
      options->run_frequency_sweep =
          ParseBoolOr(value, options->run_frequency_sweep);
    } else if (key == "--run_bandwidth_sweep") {
      options->run_bandwidth_sweep =
          ParseBoolOr(value, options->run_bandwidth_sweep);
    } else if (key == "--run_subscriber_scaling") {
      options->run_subscriber_scaling =
          ParseBoolOr(value, options->run_subscriber_scaling);
    } else if (key == "--run_publisher_scaling") {
      options->run_publisher_scaling =
          ParseBoolOr(value, options->run_publisher_scaling);
    } else if (key == "--run_cpu_interference") {
      options->run_cpu_interference =
          ParseBoolOr(value, options->run_cpu_interference);
    } else if (key == "--run_flood_mode_comparison") {
      options->run_flood_mode_comparison =
          ParseBoolOr(value, options->run_flood_mode_comparison);
    } else if (key == "--run_payload_sweep_comparison") {
      options->run_payload_sweep_comparison =
          ParseBoolOr(value, options->run_payload_sweep_comparison);
    } else if (key == "--run_fanout_scaling_comparison") {
      options->run_fanout_scaling_comparison =
          ParseBoolOr(value, options->run_fanout_scaling_comparison);
    } else if (key == "--flood_duration_s") {
      options->flood_duration_s = std::max(1, ParseIntOr(value, options->flood_duration_s));
    } else if (key == "--flood_msg_payload_bytes") {
      options->flood_msg_payload_bytes =
          std::max(1, ParseIntOr(value, options->flood_msg_payload_bytes));
    } else if (key == "--flood_bw_payload_mb") {
      options->flood_bw_payload_mb =
          std::max(1, ParseIntOr(value, options->flood_bw_payload_mb));
    } else if (key == "--payload_sweep_frequency_hz") {
      options->payload_sweep_frequency_hz =
          std::max(1, ParseIntOr(value, options->payload_sweep_frequency_hz));
    } else if (key == "--payload_sweep_duration_s") {
      options->payload_sweep_duration_s =
          std::max(1, ParseIntOr(value, options->payload_sweep_duration_s));
    } else if (key == "--payload_sweep_sizes_mb") {
      std::vector<int> sizes = ParseCpuSet(value);
      sizes.erase(std::remove_if(sizes.begin(), sizes.end(),
                                 [](int size) { return size <= 0; }),
                  sizes.end());
      if (sizes.empty()) {
        if (error != nullptr) {
          *error = "payload sweep sizes must contain a positive value";
        }
        return false;
      }
      options->payload_sweep_sizes_mb = std::move(sizes);
    } else if (key == "--fanout_frequency_hz") {
      options->fanout_frequency_hz =
          std::max(1, ParseIntOr(value, options->fanout_frequency_hz));
    } else if (key == "--fanout_payload_mb") {
      options->fanout_payload_mb =
          std::max(1, ParseIntOr(value, options->fanout_payload_mb));
    } else if (key == "--fanout_duration_s") {
      options->fanout_duration_s =
          std::max(1, ParseIntOr(value, options->fanout_duration_s));
    } else {
      if (error != nullptr) {
        *error = "unknown argument: " + key;
      }
      return false;
    }
  }

  if (options->quick_mode) {
    options->frequency_case_duration_s = 1;
    options->bandwidth_case_duration_s = 1;
    options->scaling_case_duration_s = 1;
    options->cpu_interference_duration_s = 2;
    options->frequency_step_hz = std::max(options->frequency_step_hz, 1000);
    options->bandwidth_step_mb = std::max(options->bandwidth_step_mb, 3);
    options->max_subscribers = std::min(options->max_subscribers, 4);
    options->max_publishers = std::min(options->max_publishers, 4);
  }

  if (options->frequency_start_hz > options->frequency_end_hz) {
    std::swap(options->frequency_start_hz, options->frequency_end_hz);
  }
  if (options->bandwidth_min_mb > options->bandwidth_max_mb) {
    std::swap(options->bandwidth_min_mb, options->bandwidth_max_mb);
  }
  return true;
}

void PrintUsage() {
  std::cout
      << "Cyber RT Benchmark Suite\n"
      << "Usage: benchmark_monitor [--key=value] [--quick]\n"
      << "Common options:\n"
      << "  --output_json=<path>\n"
      << "  --cpu_set=0,1,2\n"
      << "  --benchmark_pub_bin=<path>\n"
      << "  --benchmark_sub_bin=<path>\n"
      << "  --process_case_timeout_s=<seconds>\n"
      << "  --use_real_inter_process=true|false\n"
      << "  --quick\n"
      << "  --enable_long_run=true|false\n"
      << "  --long_run_seconds=<seconds>\n";
}

bool ShouldUseDifferentProcess(CoverageMode coverage) {
  return coverage == CoverageMode::kInterProcess;
}

bool ShouldUseDifferentHost(CoverageMode coverage) {
  return coverage == CoverageMode::kInterHost;
}

std::string ReceiverHostIpForCoverage(CoverageMode coverage, int receiver_index) {
  if (!ShouldUseDifferentHost(coverage)) {
    return common::GlobalData::Instance()->HostIp();
  }
  const int host_suffix = 10 + (receiver_index % 200);
  std::ostringstream oss;
  oss << "10.253.1." << host_suffix;
  return oss.str();
}

void StartCpuInterference(CpuInterferenceController* controller,
                          int load_percent, const std::vector<int>& cpu_set) {
  if (controller == nullptr || load_percent <= 0 || cpu_set.empty()) {
    return;
  }
  controller->run.store(true, std::memory_order_release);
  const int bounded_load = std::max(1, std::min(99, load_percent));
  for (size_t i = 0; i < cpu_set.size(); ++i) {
    controller->workers.emplace_back([controller, bounded_load, cpu = cpu_set[i]]() {
      (void)PinCurrentThreadToCpu(cpu);
      const uint64_t cycle_ns = 1000000ULL;
      const uint64_t busy_ns =
          (cycle_ns * static_cast<uint64_t>(bounded_load)) / 100ULL;
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

class BenchmarkSuiteRunner {
 public:
  explicit BenchmarkSuiteRunner(BenchmarkOptions options)
      : options_(std::move(options)) {}

  bool Run() {
    results_.clear();
    case_counter_ = 0;
    if (!RunShmZeroCopyProbe()) {
      // keep running; probe failure should be visible in report but not stop suite
    }
    RunZeroCopyVsProtobufComparison();
    if (options_.run_frequency_sweep) {
      RunFrequencySweep();
    }
    if (options_.run_bandwidth_sweep) {
      RunBandwidthSweep();
    }
    if (options_.run_subscriber_scaling) {
      RunSubscriberScaling();
    }
    if (options_.run_publisher_scaling) {
      RunPublisherScaling();
    }
    if (options_.run_cpu_interference) {
      RunCpuInterferenceSweep();
    }
    if (options_.enable_long_run) {
      RunLongRunCase();
    }
    return ExportResults();
  }

  const std::vector<BenchmarkCaseResult>& results() const { return results_; }

 private:
  bool RunShmZeroCopyProbe() {
    if (options_.use_real_inter_process) {
      return RunShmZeroCopyProbeViaWorkers();
    }

    BenchmarkCaseConfig config;
    config.scenario = ScenarioKind::kShmZeroCopyProbe;
    config.coverage = CoverageMode::kInterProcess;
    config.message_type = MessageType::kPod;
    config.topology = TopologyMode::kOnePubOneSub;
    config.publishers = 1;
    config.subscribers = 1;
    config.frequency_hz = 1;
    config.payload_bytes = 1024;
    config.duration_s = 1;

    BenchmarkCaseResult result;
    result.config = config;

    const std::string channel =
        "perf/shm_zero_copy_probe/" + std::to_string(++case_counter_);
    const int process_id = common::GlobalData::Instance()->ProcessId();
    const std::string host_ip = common::GlobalData::Instance()->HostIp();

    auto pub_attr = BuildRoleAttributes(channel, "probe_pub", host_ip, process_id,
                                        case_counter_ * 17 + 1);
    auto sub_attr = BuildRoleAttributes(channel, "probe_sub", host_ip, process_id + 1,
                                        case_counter_ * 17 + 2);

    std::atomic<bool> received{false};
    auto receiver = transport::Transport::Instance()->CreateReceiver<transport::PodMessage>(
        sub_attr,
        [&](const std::shared_ptr<transport::PodMessage>& msg,
            const transport::MessageInfo&, const apollo::cyber::proto::RoleAttributes&) {
          if (msg != nullptr) {
            received.store(true, std::memory_order_release);
          }
        },
        apollo::cyber::proto::OptionalMode::SHM);
    if (receiver == nullptr) {
      result.success = false;
      result.error_message = "failed to create SHM probe receiver";
      results_.push_back(std::move(result));
      return false;
    }

    auto transmitter =
        transport::Transport::Instance()->CreateTransmitter<transport::PodMessage>(
            pub_attr, apollo::cyber::proto::OptionalMode::SHM);
    if (transmitter == nullptr) {
      result.success = false;
      result.error_message = "failed to create SHM probe transmitter";
      receiver->Disable();
      results_.push_back(std::move(result));
      return false;
    }

    result.shm_loan_supported = transmitter->IsLoanSupported();
    const std::string payload(config.payload_bytes, 'z');
    transport::PodChunkHeader header = transport::MakeImagePodChunkHeader(
        MonotonicRawNowNs(), 1, 1, 1, 1, 0,
        static_cast<uint32_t>(payload.size()));

    bool published = false;
    if (result.shm_loan_supported) {
      transport::LoanedMessage<transport::PodMessage> loaned;
      const std::size_t required = transport::PodChunkTotalSize(payload.size());
      if (transmitter->Loan(required, &loaned)) {
        std::size_t written = 0;
        if (transport::BuildPodChunk(header, payload.data(), payload.size(),
                                     loaned.data(), loaned.capacity(), &written) &&
            loaned.set_size(written)) {
          published = transmitter->Publish(std::move(loaned));
        }
      }
    }
    if (!published) {
      auto msg = std::make_shared<transport::PodMessage>(header, payload.data(),
                                                         payload.size());
      published = transmitter->Transmit(msg);
    }

    for (int i = 0; i < 20; ++i) {
      if (received.load(std::memory_order_acquire)) {
        break;
      }
      SleepNs(50000000ULL);  // 50ms
    }

    result.shm_profile_recorded =
        transport::TransportProfileRecorder::Instance()
            ->GenerateToml()
            .find("name = \"" + channel + "\"") != std::string::npos;
    result.success = published && received.load(std::memory_order_acquire) &&
                     result.shm_profile_recorded;
    if (!result.success) {
      result.error_message =
          "SHM probe failed (publish/receive/profile criteria not all met)";
    }
    result.notes =
        "SHM zero-copy probe checks loan support, data delivery, and SHM profile record.";

    transmitter->Disable();
    receiver->Disable();
    results_.push_back(std::move(result));
    return results_.back().success;
  }

  bool RunShmZeroCopyProbeViaWorkers() {
    BenchmarkCaseConfig config;
    config.scenario = ScenarioKind::kShmZeroCopyProbe;
    config.coverage = CoverageMode::kInterProcess;
    config.message_type = MessageType::kPod;
    config.topology = TopologyMode::kOnePubOneSub;
    config.publishers = 1;
    config.subscribers = 1;
    config.frequency_hz = 1;
    config.payload_bytes = 1024;
    config.duration_s = 2;

    BenchmarkCaseResult result;
    result.config = config;
    if (options_.benchmark_pub_binary.empty() ||
        options_.benchmark_sub_binary.empty()) {
      result.success = false;
      result.error_message =
          "benchmark_pub/benchmark_sub binary path is not configured";
      results_.push_back(std::move(result));
      return false;
    }
    if (!FileExists(options_.benchmark_pub_binary) ||
        !FileExists(options_.benchmark_sub_binary)) {
      result.success = false;
      result.error_message =
          "benchmark_pub or benchmark_sub binary does not exist";
      results_.push_back(std::move(result));
      return false;
    }

    const uint64_t case_suffix = ++case_counter_;
    const std::string channel =
        "perf/shm_zero_copy_probe/" + std::to_string(case_suffix);
    const std::string case_dir = CreateCaseTempDir(case_suffix);
    if (case_dir.empty()) {
      result.success = false;
      result.error_message = "failed to create temporary case directory";
      results_.push_back(std::move(result));
      return false;
    }
    const std::string sub_result = JoinPath(case_dir, "sub_probe.kv");
    const std::string pub_result = JoinPath(case_dir, "pub_probe.kv");

    const uint64_t start_ns =
        MonotonicRawNowNs() +
        static_cast<uint64_t>(std::max(100, options_.startup_wait_ms)) *
            kOneMillisecondNs;
    const int sub_cpu = options_.cpu_set.empty() ? 0 : options_.cpu_set.front();
    const int pub_cpu = options_.cpu_set.size() > 1
                            ? options_.cpu_set[1]
                            : (options_.cpu_set.empty() ? 0 : options_.cpu_set.front());

    std::vector<ChildProcess> children;
    {
      ChildProcess sub_child;
      std::vector<std::string> sub_args = {
          options_.benchmark_sub_binary,
          "--worker_mode=shm_probe",
          "--coverage=inter_process",
          "--channel=" + channel,
          "--node_name=probe_sub",
          "--publishers=1",
          "--subscriber_index=0",
          "--duration_s=" + std::to_string(config.duration_s),
          "--start_ns=" + std::to_string(start_ns),
          "--result_path=" + sub_result,
          "--latency_dump_path=" + JoinPath(case_dir, "sub_probe.lat"),
          "--cpu=" + std::to_string(sub_cpu),
      };
      std::string spawn_error;
      if (!SpawnProcess(sub_args, &sub_child, &spawn_error)) {
        result.success = false;
        result.error_message = "failed to spawn shm probe subscriber: " + spawn_error;
        RemoveFileIfExists(sub_result);
        RemoveFileIfExists(pub_result);
        (void)rmdir(case_dir.c_str());
        results_.push_back(std::move(result));
        return false;
      }
      sub_child.role = "sub_probe";
      sub_child.result_path = sub_result;
      children.push_back(sub_child);
    }
    {
      ChildProcess pub_child;
      std::vector<std::string> pub_args = {
          options_.benchmark_pub_binary,
          "--worker_mode=shm_probe",
          "--coverage=inter_process",
          "--channel=" + channel,
          "--node_name=probe_pub",
          "--publisher_index=0",
          "--payload_bytes=" + std::to_string(config.payload_bytes),
          "--duration_s=" + std::to_string(config.duration_s),
          "--start_ns=" + std::to_string(start_ns),
          "--result_path=" + pub_result,
          "--cpu=" + std::to_string(pub_cpu),
      };
      std::string spawn_error;
      if (!SpawnProcess(pub_args, &pub_child, &spawn_error)) {
        result.success = false;
        result.error_message = "failed to spawn shm probe publisher: " + spawn_error;
        (void)kill(children.front().pid, SIGKILL);
        (void)waitpid(children.front().pid, nullptr, 0);
        RemoveFileIfExists(sub_result);
        RemoveFileIfExists(pub_result);
        (void)rmdir(case_dir.c_str());
        results_.push_back(std::move(result));
        return false;
      }
      pub_child.role = "pub_probe";
      pub_child.result_path = pub_result;
      children.push_back(pub_child);
    }

    std::string wait_error;
    if (!WaitForChildren(&children,
                         std::max(options_.process_case_timeout_s, config.duration_s + 20),
                         &wait_error)) {
      result.success = false;
      result.error_message = "shm probe workers failed: " + wait_error;
      RemoveFileIfExists(sub_result);
      RemoveFileIfExists(pub_result);
      (void)rmdir(case_dir.c_str());
      results_.push_back(std::move(result));
      return false;
    }

    std::unordered_map<std::string, std::string> pub_kv;
    std::unordered_map<std::string, std::string> sub_kv;
    if (!ReadKvFile(pub_result, &pub_kv) || !ReadKvFile(sub_result, &sub_kv)) {
      result.success = false;
      result.error_message = "failed to read shm probe worker results";
      RemoveFileIfExists(sub_result);
      RemoveFileIfExists(pub_result);
      (void)rmdir(case_dir.c_str());
      results_.push_back(std::move(result));
      return false;
    }

    const bool published = ParseIntOr(pub_kv["probe_published"], 0) == 1;
    const bool loan_supported = ParseIntOr(pub_kv["probe_loan_supported"], 0) == 1;
    const bool received = ParseIntOr(sub_kv["probe_received"], 0) == 1;
    const uint64_t borrowed_messages =
        ParseUInt64Or(sub_kv["zero_copy_borrowed_messages"], 0);
    const uint64_t copy_count = ParseUInt64Or(sub_kv["zero_copy_copy_count"], 0);
    const uint64_t pub_ptr = ParseUInt64Or(pub_kv["probe_pub_ptr_value"], 0);
    const uint64_t sub_ptr = ParseUInt64Or(sub_kv["probe_sub_ptr_value"], 0);
    const uint64_t pub_ptr_from_header =
        ParseUInt64Or(sub_kv["probe_pub_ptr_from_header"], 0);

    result.shm_loan_supported = loan_supported;
    result.shm_profile_recorded = borrowed_messages > 0 && copy_count == 0;
    const bool zero_copy_verified = borrowed_messages > 0 && copy_count == 0;
    result.success = published && received;
    if (!result.success) {
      result.error_message =
          "SHM zero-copy probe failed (publish/receive criteria)";
    }
    std::ostringstream notes;
    notes << "real_multi_process=true"
          << " | borrowed_messages=" << borrowed_messages
          << " | copy_count=" << copy_count
          << " | loan_supported=" << (loan_supported ? "true" : "false")
          << " | zero_copy_verified=" << (zero_copy_verified ? "true" : "false")
          << " | pub_ptr=" << pub_ptr
          << " | sub_ptr=" << sub_ptr
          << " | pub_ptr_from_header=" << pub_ptr_from_header;
    result.notes = notes.str();

    RemoveFileIfExists(sub_result);
    RemoveFileIfExists(pub_result);
    RemoveFileIfExists(JoinPath(case_dir, "sub_probe.lat"));
    (void)rmdir(case_dir.c_str());

    results_.push_back(std::move(result));
    return results_.back().success;
  }

  void RunZeroCopyVsProtobufComparison() {
    BenchmarkCaseConfig base;
    base.scenario = ScenarioKind::kZeroCopyVsProtobuf;
    base.coverage = CoverageMode::kInterProcess;
    base.topology = TopologyMode::kOnePubOneSub;
    base.publishers = 1;
    base.subscribers = 1;
    base.frequency_hz = options_.scaling_frequency_hz;
    base.payload_bytes = options_.scaling_payload_bytes;
    base.duration_s = options_.scaling_case_duration_s;

    BenchmarkCaseConfig protobuf_case = base;
    protobuf_case.message_type = MessageType::kProtobuf;
    BenchmarkCaseResult protobuf_result = RunSingleCase(protobuf_case);
    results_.push_back(protobuf_result);

    BenchmarkCaseConfig pod_case = base;
    pod_case.message_type = MessageType::kPod;
    BenchmarkCaseResult pod_result = RunSingleCase(pod_case);
    results_.push_back(pod_result);

    const int iceoryx_payload_cap_mb = static_cast<int>(
        (transport::kIceoryxChunkPayloadCapacity -
         sizeof(transport::PodChunkHeader)) /
        kOneMegabyte);
    const int max_common_bandwidth_mb =
        std::max(1, std::min(options_.bandwidth_max_mb, iceoryx_payload_cap_mb));
    const int min_common_bandwidth_mb =
        std::max(1, std::min(options_.bandwidth_min_mb, max_common_bandwidth_mb));

    auto best_bandwidth_for = [&](MessageType type,
                                  BenchmarkCaseResult* best_result) -> bool {
      bool found = false;
      for (int mb = min_common_bandwidth_mb; mb <= max_common_bandwidth_mb;
           mb += options_.bandwidth_step_mb) {
        BenchmarkCaseConfig cfg = base;
        cfg.message_type = type;
        cfg.frequency_hz = options_.bandwidth_frequency_hz;
        cfg.payload_bytes = mb * static_cast<int>(kOneMegabyte);
        cfg.duration_s = options_.bandwidth_case_duration_s;
        BenchmarkCaseResult current = RunSingleCase(cfg);
        if (!current.success || current.reliability.total_loss != 0 ||
            current.throughput.send_failures != 0) {
          continue;
        }
        if (!found ||
            current.throughput.mb_per_s > best_result->throughput.mb_per_s) {
          *best_result = current;
          found = true;
        }
      }
      return found;
    };

    BenchmarkCaseResult protobuf_best;
    BenchmarkCaseResult pod_best;
    const bool protobuf_best_found =
        best_bandwidth_for(MessageType::kProtobuf, &protobuf_best);
    const bool pod_best_found = best_bandwidth_for(MessageType::kPod, &pod_best);
    if (protobuf_best_found) {
      protobuf_best.notes = "comparison_max_bandwidth_point";
      results_.push_back(protobuf_best);
    }
    if (pod_best_found) {
      pod_best.notes = "comparison_max_bandwidth_point";
      results_.push_back(pod_best);
    }

    if (options_.run_flood_mode_comparison) {
      const int flood_bw_payload_mb = std::max(
          1, std::min(options_.flood_bw_payload_mb, max_common_bandwidth_mb));
      for (MessageType type : {MessageType::kProtobuf, MessageType::kPod}) {
        BenchmarkCaseConfig msg_cfg = base;
        msg_cfg.scenario = ScenarioKind::kFloodMode;
        msg_cfg.message_type = type;
        msg_cfg.frequency_hz = 0;
        msg_cfg.payload_bytes = options_.flood_msg_payload_bytes;
        msg_cfg.duration_s = options_.flood_duration_s;
        BenchmarkCaseResult msg_result = RunSingleCase(msg_cfg);
        msg_result.notes += " | metric_focus=max_msgps";
        results_.push_back(std::move(msg_result));

        BenchmarkCaseConfig bw_cfg = base;
        bw_cfg.scenario = ScenarioKind::kFloodMode;
        bw_cfg.message_type = type;
        bw_cfg.frequency_hz = 0;
        bw_cfg.payload_bytes = flood_bw_payload_mb * static_cast<int>(kOneMegabyte);
        bw_cfg.duration_s = options_.flood_duration_s;
        BenchmarkCaseResult bw_result = RunSingleCase(bw_cfg);
        bw_result.notes += " | metric_focus=max_mbps";
        results_.push_back(std::move(bw_result));
      }
    }

    if (options_.run_payload_sweep_comparison) {
      for (int payload_mb : options_.payload_sweep_sizes_mb) {
        BenchmarkCaseConfig pb_cfg = base;
        pb_cfg.scenario = ScenarioKind::kPayloadSweep;
        pb_cfg.message_type = MessageType::kProtobuf;
        pb_cfg.frequency_hz = options_.payload_sweep_frequency_hz;
        pb_cfg.payload_bytes = payload_mb * static_cast<int>(kOneMegabyte);
        pb_cfg.duration_s = options_.payload_sweep_duration_s;
        results_.push_back(RunSingleCase(pb_cfg));

        BenchmarkCaseConfig pod_cfg = base;
        pod_cfg.scenario = ScenarioKind::kPayloadSweep;
        pod_cfg.message_type = MessageType::kPod;
        pod_cfg.frequency_hz = options_.payload_sweep_frequency_hz;
        pod_cfg.duration_s = options_.payload_sweep_duration_s;
        if (payload_mb <= iceoryx_payload_cap_mb) {
          pod_cfg.payload_bytes = payload_mb * static_cast<int>(kOneMegabyte);
          results_.push_back(RunSingleCase(pod_cfg));
        } else {
          BenchmarkCaseResult skipped;
          skipped.config = pod_cfg;
          skipped.config.payload_bytes =
              payload_mb * static_cast<int>(kOneMegabyte);
          skipped.success = true;
          skipped.notes = "skipped_for_iceoryx_payload_cap_mb=" +
                          std::to_string(iceoryx_payload_cap_mb);
          results_.push_back(std::move(skipped));
        }
      }
    }

    if (options_.run_fanout_scaling_comparison) {
      const int fanout_payload_mb =
          std::max(1, std::min(options_.fanout_payload_mb, max_common_bandwidth_mb));
      for (int subscribers : options_.fanout_subscribers) {
        for (MessageType type : {MessageType::kProtobuf, MessageType::kPod}) {
          BenchmarkCaseConfig fan_cfg = base;
          fan_cfg.scenario = ScenarioKind::kFanoutScaling;
          fan_cfg.topology = TopologyMode::kOnePubMSub;
          fan_cfg.publishers = 1;
          fan_cfg.subscribers = std::max(1, subscribers);
          fan_cfg.frequency_hz = options_.fanout_frequency_hz;
          fan_cfg.payload_bytes = fanout_payload_mb * static_cast<int>(kOneMegabyte);
          fan_cfg.duration_s = options_.fanout_duration_s;
          fan_cfg.message_type = type;
          results_.push_back(RunSingleCase(fan_cfg));
        }
      }
    }

    BenchmarkCaseResult summary;
    summary.config = base;
    summary.success = protobuf_result.success && pod_result.success;
    std::ostringstream notes;
    notes << "comparison=protobuf_shm_vs_pod_iceoryx";
    if (!protobuf_result.success || !pod_result.success) {
      notes << " | latency_case_failed";
      summary.error_message =
          "zero-copy vs protobuf latency baseline case failed";
    } else {
      notes << " | latency_p50_ns.protobuf=" << protobuf_result.latency.p50_ns
            << " | latency_p50_ns.pod=" << pod_result.latency.p50_ns
            << " | latency_p95_ns.protobuf=" << protobuf_result.latency.p95_ns
            << " | latency_p95_ns.pod=" << pod_result.latency.p95_ns
            << " | latency_p99_ns.protobuf=" << protobuf_result.latency.p99_ns
            << " | latency_p99_ns.pod=" << pod_result.latency.p99_ns
            << " | mbps.protobuf=" << protobuf_result.throughput.mb_per_s
            << " | mbps.pod=" << pod_result.throughput.mb_per_s
            << " | cpu_util.protobuf="
            << protobuf_result.resource.cpu_utilization_percent
            << " | cpu_util.pod=" << pod_result.resource.cpu_utilization_percent
            << " | rss_end_kb.protobuf=" << protobuf_result.resource.rss_kb_end
            << " | rss_end_kb.pod=" << pod_result.resource.rss_kb_end
            << " | sys_ctx_switches.protobuf="
            << protobuf_result.resource.involuntary_context_switches
            << " | sys_ctx_switches.pod="
            << pod_result.resource.involuntary_context_switches;
    }
    if (!protobuf_best_found || !pod_best_found) {
      notes << " | max_bandwidth_case_incomplete";
    } else {
      const double max_bw_gain =
          SafeDiv(pod_best.throughput.mb_per_s, protobuf_best.throughput.mb_per_s);
      notes << " | max_bandwidth_mb_s.protobuf="
            << protobuf_best.throughput.mb_per_s
            << " | max_bandwidth_mb_s.pod=" << pod_best.throughput.mb_per_s
            << " | max_bandwidth_gain.pod_over_protobuf=" << max_bw_gain;
    }
    notes << " | max_common_bandwidth_payload_mb=" << max_common_bandwidth_mb;
    notes << " | zero_copy_available="
          << ((pod_result.shm_loan_supported && pod_result.shm_profile_recorded)
                  ? "true"
                  : "false");
    summary.notes = notes.str();
    results_.push_back(std::move(summary));
  }

  void RunFrequencySweep() {
    for (CoverageMode coverage : AllCoverages()) {
      int highest_stable_hz = 0;
      for (int hz = options_.frequency_start_hz; hz <= options_.frequency_end_hz;
           hz += options_.frequency_step_hz) {
        BenchmarkCaseConfig config;
        config.scenario = ScenarioKind::kFrequencySweep;
        config.coverage = coverage;
        config.topology = TopologyMode::kOnePubOneSub;
        config.publishers = 1;
        config.subscribers = 1;
        config.frequency_hz = hz;
        config.payload_bytes = options_.frequency_payload_bytes;
        config.duration_s = options_.frequency_case_duration_s;

        BenchmarkCaseResult result = RunSingleCase(config);
        if (result.success && result.reliability.total_loss == 0 &&
            result.throughput.send_failures == 0) {
          highest_stable_hz = hz;
        } else if (!result.success) {
          result.notes += " | run failed";
        } else {
          result.notes += " | unstable point";
        }
        results_.push_back(std::move(result));
      }

      BenchmarkCaseResult summary;
      summary.config.scenario = ScenarioKind::kFrequencySweep;
      summary.config.coverage = coverage;
      summary.success = true;
      summary.notes = "frequency_limit_hz=" + std::to_string(highest_stable_hz);
      results_.push_back(std::move(summary));
    }
  }

  void RunBandwidthSweep() {
    for (CoverageMode coverage : AllCoverages()) {
      int highest_stable_mb = 0;
      for (int mb = options_.bandwidth_min_mb; mb <= options_.bandwidth_max_mb;
           mb += options_.bandwidth_step_mb) {
        BenchmarkCaseConfig config;
        config.scenario = ScenarioKind::kBandwidthSweep;
        config.coverage = coverage;
        config.topology = TopologyMode::kOnePubOneSub;
        config.publishers = 1;
        config.subscribers = 1;
        config.frequency_hz = options_.bandwidth_frequency_hz;
        config.payload_bytes = mb * static_cast<int>(kOneMegabyte);
        config.duration_s = options_.bandwidth_case_duration_s;

        BenchmarkCaseResult result = RunSingleCase(config);
        if (result.success && result.reliability.total_loss == 0 &&
            result.throughput.send_failures == 0) {
          highest_stable_mb = mb;
        }
        results_.push_back(std::move(result));
      }

      BenchmarkCaseResult summary;
      summary.config.scenario = ScenarioKind::kBandwidthSweep;
      summary.config.coverage = coverage;
      summary.success = true;
      summary.notes = "bandwidth_limit_mb=" + std::to_string(highest_stable_mb);
      results_.push_back(std::move(summary));
    }
  }

  void RunSubscriberScaling() {
    for (CoverageMode coverage : AllCoverages()) {
      for (int subscribers = 1; subscribers <= options_.max_subscribers;
           ++subscribers) {
        BenchmarkCaseConfig config;
        config.scenario = ScenarioKind::kSubscriberScaling;
        config.coverage = coverage;
        config.topology = TopologyMode::kOnePubMSub;
        config.publishers = 1;
        config.subscribers = subscribers;
        config.frequency_hz = options_.scaling_frequency_hz;
        config.payload_bytes = options_.scaling_payload_bytes;
        config.duration_s = options_.scaling_case_duration_s;
        results_.push_back(RunSingleCase(config));
      }
    }
  }

  void RunPublisherScaling() {
    for (CoverageMode coverage : AllCoverages()) {
      for (int publishers = 1; publishers <= options_.max_publishers;
           ++publishers) {
        BenchmarkCaseConfig config;
        config.scenario = ScenarioKind::kPublisherScaling;
        config.coverage = coverage;
        config.topology = TopologyMode::kNPubOneSub;
        config.publishers = publishers;
        config.subscribers = 1;
        config.frequency_hz = options_.scaling_frequency_hz;
        config.payload_bytes = options_.scaling_payload_bytes;
        config.duration_s = options_.scaling_case_duration_s;
        results_.push_back(RunSingleCase(config));
      }
    }
  }

  void RunCpuInterferenceSweep() {
    for (CoverageMode coverage : AllCoverages()) {
      for (int level : options_.cpu_interference_levels) {
        BenchmarkCaseConfig config;
        config.scenario = ScenarioKind::kCpuInterference;
        config.coverage = coverage;
        config.topology = TopologyMode::kOnePubOneSub;
        config.publishers = 1;
        config.subscribers = 1;
        config.frequency_hz = options_.cpu_interference_frequency_hz;
        config.payload_bytes = options_.cpu_interference_payload_bytes;
        config.duration_s = options_.cpu_interference_duration_s;
        config.cpu_interference_percent = level;
        results_.push_back(RunSingleCase(config));
      }
    }
  }

  void RunLongRunCase() {
    BenchmarkCaseConfig config;
    config.scenario = ScenarioKind::kLongRun;
    config.coverage = CoverageMode::kInterProcess;
    config.topology = TopologyMode::kOnePubOneSub;
    config.publishers = 1;
    config.subscribers = 1;
    config.frequency_hz = options_.long_run_frequency_hz;
    config.payload_bytes = options_.long_run_payload_bytes;
    config.duration_s = options_.long_run_seconds;
    results_.push_back(RunSingleCase(config));
  }

  BenchmarkCaseResult RunSingleCase(const BenchmarkCaseConfig& config) {
    if (options_.use_real_inter_process &&
        (config.coverage == CoverageMode::kInterProcess ||
         config.coverage == CoverageMode::kInterHost)) {
      return RunSingleCaseViaWorkers(config);
    }
    return RunSingleCaseInProcess(config);
  }

  BenchmarkCaseResult RunSingleCaseViaWorkers(const BenchmarkCaseConfig& config) {
    BenchmarkCaseResult result;
    result.config = config;
    result.message_pool_depth = ComputeMessagePoolDepth(options_, config);
    const int worker_pool_depth =
        config.payload_bytes >= static_cast<int>(8 * kOneMegabyte)
            ? std::min(result.message_pool_depth, 2)
            : result.message_pool_depth;
    if (options_.benchmark_pub_binary.empty() ||
        options_.benchmark_sub_binary.empty()) {
      result.success = false;
      result.error_message =
          "benchmark_pub/benchmark_sub binary path is not configured";
      return result;
    }
    if (!FileExists(options_.benchmark_pub_binary) ||
        !FileExists(options_.benchmark_sub_binary)) {
      result.success = false;
      result.error_message =
          "benchmark_pub or benchmark_sub binary does not exist";
      return result;
    }

    const uint64_t case_suffix = ++case_counter_;
    const std::string channel_name = MakeChannelName(config, case_suffix);
    const std::string case_dir = CreateCaseTempDir(case_suffix);
    if (case_dir.empty()) {
      result.success = false;
      result.error_message = "failed to create temporary case directory";
      return result;
    }

    std::vector<std::string> sub_result_paths;
    std::vector<std::string> sub_latency_paths;
    std::vector<std::string> pub_result_paths;
    sub_result_paths.reserve(static_cast<size_t>(config.subscribers));
    sub_latency_paths.reserve(static_cast<size_t>(config.subscribers));
    pub_result_paths.reserve(static_cast<size_t>(config.publishers));

    const uint64_t start_ns =
        MonotonicRawNowNs() +
        static_cast<uint64_t>(std::max(100, options_.startup_wait_ms)) *
            kOneMillisecondNs;
    const int timeout_s =
        std::max(options_.process_case_timeout_s,
                 config.duration_s + options_.startup_wait_ms / 1000 +
                     options_.cooldown_wait_ms / 1000 + 20);
    const std::string cpu_set_text = JoinCpuSet(options_.cpu_set);
    const std::string worker_transport_mode = WorkerTransportModeForCase(config);

    std::vector<ChildProcess> children;
    auto kill_and_reap_children = [&children]() {
      for (const auto& child : children) {
        if (child.pid > 0) {
          (void)kill(child.pid, SIGKILL);
        }
      }
      for (const auto& child : children) {
        if (child.pid > 0) {
          (void)waitpid(child.pid, nullptr, 0);
        }
      }
    };
    auto cleanup_case_files = [&]() {
      for (const auto& path : sub_result_paths) {
        RemoveFileIfExists(path);
      }
      for (const auto& path : sub_latency_paths) {
        RemoveFileIfExists(path);
      }
      for (const auto& path : pub_result_paths) {
        RemoveFileIfExists(path);
      }
      (void)rmdir(case_dir.c_str());
    };

    for (int sub_index = 0; sub_index < config.subscribers; ++sub_index) {
      const std::string sub_result =
          JoinPath(case_dir, "sub_" + std::to_string(sub_index) + ".kv");
      const std::string sub_latency =
          JoinPath(case_dir, "sub_" + std::to_string(sub_index) + ".lat");
      sub_result_paths.push_back(sub_result);
      sub_latency_paths.push_back(sub_latency);

      const int cpu =
          options_.cpu_set.empty()
              ? 0
              : options_.cpu_set[static_cast<size_t>(
                    sub_index % static_cast<int>(options_.cpu_set.size()))];
      std::vector<std::string> args = {
          options_.benchmark_sub_binary,
          "--worker_mode=benchmark",
          "--coverage=" + ToString(config.coverage),
          "--message_type=" + ToString(config.message_type),
          "--transport_mode=" + worker_transport_mode,
          "--channel=" + channel_name,
          "--node_name=perf_sub_worker_" + std::to_string(sub_index),
          "--host_ip=" + ReceiverHostIpForCoverage(config.coverage, sub_index),
          "--publishers=" + std::to_string(config.publishers),
          "--subscriber_index=" + std::to_string(sub_index),
          "--duration_s=" + std::to_string(config.duration_s),
          "--start_ns=" + std::to_string(start_ns),
          "--cpu_interference_percent=" +
              std::to_string(config.cpu_interference_percent),
          "--cooldown_wait_ms=" + std::to_string(options_.cooldown_wait_ms),
          "--latency_sample_cap=" + std::to_string(options_.latency_sample_cap),
          "--result_path=" + sub_result,
          "--latency_dump_path=" + sub_latency,
          "--cpu=" + std::to_string(cpu),
      };
      ChildProcess child;
      std::string spawn_error;
      if (!SpawnProcess(args, &child, &spawn_error)) {
        result.success = false;
        result.error_message = "failed to spawn subscriber worker: " + spawn_error;
        kill_and_reap_children();
        cleanup_case_files();
        return result;
      }
      child.role = "sub";
      child.result_path = sub_result;
      children.push_back(child);
    }

    for (int pub_index = 0; pub_index < config.publishers; ++pub_index) {
      const std::string pub_result =
          JoinPath(case_dir, "pub_" + std::to_string(pub_index) + ".kv");
      pub_result_paths.push_back(pub_result);
      const int cpu =
          options_.cpu_set.empty()
              ? 0
              : options_.cpu_set[static_cast<size_t>(
                    (config.subscribers + pub_index) %
                    static_cast<int>(options_.cpu_set.size()))];
      std::vector<std::string> args = {
          options_.benchmark_pub_binary,
          "--worker_mode=benchmark",
          "--coverage=" + ToString(config.coverage),
          "--message_type=" + ToString(config.message_type),
          "--transport_mode=" + worker_transport_mode,
          "--channel=" + channel_name,
          "--node_name=perf_pub_worker_" + std::to_string(pub_index),
          "--publisher_index=" + std::to_string(pub_index),
          "--frequency_hz=" + std::to_string(config.frequency_hz),
          "--payload_bytes=" + std::to_string(config.payload_bytes),
          "--duration_s=" + std::to_string(config.duration_s),
          "--start_ns=" + std::to_string(start_ns),
          "--cpu_interference_percent=" +
              std::to_string(config.cpu_interference_percent),
          "--message_pool_depth=" + std::to_string(worker_pool_depth),
          "--result_path=" + pub_result,
          "--cpu=" + std::to_string(cpu),
      };
      ChildProcess child;
      std::string spawn_error;
      if (!SpawnProcess(args, &child, &spawn_error)) {
        result.success = false;
        result.error_message = "failed to spawn publisher worker: " + spawn_error;
        kill_and_reap_children();
        cleanup_case_files();
        return result;
      }
      child.role = "pub";
      child.result_path = pub_result;
      children.push_back(child);
    }

    std::string wait_error;
    if (!WaitForChildren(&children, timeout_s, &wait_error)) {
      result.success = false;
      result.error_message = "worker run failed: " + wait_error;
      cleanup_case_files();
      return result;
    }

    std::vector<uint64_t> sent_per_publisher(static_cast<size_t>(config.publishers),
                                             0);
    uint64_t total_sent = 0;
    uint64_t total_send_failures = 0;
    bool any_loan_supported = false;
    std::string pub_transport_mode_seen;
    std::string sub_transport_mode_seen;
    uint64_t total_received = 0;
    uint64_t total_received_bytes = 0;
    uint64_t total_dropped_samples = 0;
    double cpu_delta_total = 0.0;
    long rss_begin_sum = 0;
    long rss_end_sum = 0;
    long voluntary_ctx_sum = 0;
    long involuntary_ctx_sum = 0;
    std::vector<uint64_t> all_latency_samples;

    for (int pub_index = 0; pub_index < config.publishers; ++pub_index) {
      std::unordered_map<std::string, std::string> kv;
      if (!ReadKvFile(pub_result_paths[static_cast<size_t>(pub_index)], &kv)) {
        result.success = false;
        result.error_message = "failed to read publisher result file";
        cleanup_case_files();
        return result;
      }
      if (kv["status"] != "ok") {
        result.success = false;
        result.error_message = "publisher worker reported error";
        cleanup_case_files();
        return result;
      }
      const uint64_t sent = ParseUInt64Or(kv["sent_messages"], 0);
      sent_per_publisher[static_cast<size_t>(pub_index)] = sent;
      total_sent += sent;
      total_send_failures += ParseUInt64Or(kv["send_failures"], 0);
      any_loan_supported = any_loan_supported ||
                           (ParseIntOr(kv["loan_supported"], 0) == 1);
      if (pub_transport_mode_seen.empty()) {
        pub_transport_mode_seen = kv["transport_mode"];
      }
      cpu_delta_total += ParseDoubleOr(kv["cpu_delta_s"], 0.0);
      rss_begin_sum += static_cast<long>(ParseIntOr(kv["rss_kb_begin"], 0));
      rss_end_sum += static_cast<long>(ParseIntOr(kv["rss_kb_end"], 0));
      voluntary_ctx_sum +=
          static_cast<long>(ParseIntOr(kv["voluntary_ctx_switches"], 0));
      involuntary_ctx_sum +=
          static_cast<long>(ParseIntOr(kv["involuntary_ctx_switches"], 0));
    }

    uint64_t total_loss = 0;
    uint64_t max_consecutive_loss = 0;
    uint64_t duplicates = 0;
    uint64_t total_borrowed = 0;
    uint64_t total_copies = 0;

    for (int sub_index = 0; sub_index < config.subscribers; ++sub_index) {
      std::unordered_map<std::string, std::string> kv;
      if (!ReadKvFile(sub_result_paths[static_cast<size_t>(sub_index)], &kv)) {
        result.success = false;
        result.error_message = "failed to read subscriber result file";
        cleanup_case_files();
        return result;
      }
      if (kv["status"] != "ok") {
        result.success = false;
        result.error_message = "subscriber worker reported error";
        cleanup_case_files();
        return result;
      }

      total_received += ParseUInt64Or(kv["received_messages"], 0);
      total_received_bytes += ParseUInt64Or(kv["received_bytes"], 0);
      total_dropped_samples += ParseUInt64Or(kv["dropped_samples"], 0);
      cpu_delta_total += ParseDoubleOr(kv["cpu_delta_s"], 0.0);
      rss_begin_sum += static_cast<long>(ParseIntOr(kv["rss_kb_begin"], 0));
      rss_end_sum += static_cast<long>(ParseIntOr(kv["rss_kb_end"], 0));
      voluntary_ctx_sum +=
          static_cast<long>(ParseIntOr(kv["voluntary_ctx_switches"], 0));
      involuntary_ctx_sum +=
          static_cast<long>(ParseIntOr(kv["involuntary_ctx_switches"], 0));
      total_borrowed += ParseUInt64Or(kv["zero_copy_borrowed_messages"], 0);
      total_copies += ParseUInt64Or(kv["zero_copy_copy_count"], 0);
      if (sub_transport_mode_seen.empty()) {
        sub_transport_mode_seen = kv["transport_mode"];
      }

      const std::string latency_dump = kv["latency_dump_path"].empty()
                                           ? sub_latency_paths[static_cast<size_t>(sub_index)]
                                           : kv["latency_dump_path"];
      std::vector<uint64_t> latencies;
      if (!ReadLatencyDump(latency_dump, &latencies)) {
        result.success = false;
        result.error_message = "failed to read subscriber latency dump";
        cleanup_case_files();
        return result;
      }
      all_latency_samples.insert(all_latency_samples.end(), latencies.begin(),
                                 latencies.end());

      for (int pub_index = 0; pub_index < config.publishers; ++pub_index) {
        const std::string key_prefix = "tracker_" + std::to_string(pub_index) + "_";
        const bool initialized =
            ParseIntOr(kv[key_prefix + "initialized"], 0) == 1;
        const uint64_t expected_seq =
            ParseUInt64Or(kv[key_prefix + "expected_seq"], 0);
        const uint64_t internal_loss =
            ParseUInt64Or(kv[key_prefix + "internal_loss"], 0);
        const uint64_t tracker_max =
            ParseUInt64Or(kv[key_prefix + "max_consecutive_loss"], 0);
        const uint64_t duplicate =
            ParseUInt64Or(kv[key_prefix + "duplicate_or_reordered"], 0);
        const uint64_t sent =
            sent_per_publisher[static_cast<size_t>(pub_index)];
        uint64_t tail_loss = 0;
        if (!initialized) {
          tail_loss = sent;
        } else if (sent > expected_seq) {
          tail_loss = sent - expected_seq;
        }
        total_loss += internal_loss + tail_loss;
        max_consecutive_loss =
            std::max(max_consecutive_loss, std::max(tracker_max, tail_loss));
        duplicates += duplicate;
      }
    }

    result.wall_time_ns = static_cast<uint64_t>(config.duration_s) * kOneSecondNs;
    const double wall_s = SafeDiv(static_cast<double>(result.wall_time_ns), 1e9);
    result.throughput.sent_messages = total_sent;
    result.throughput.send_failures = total_send_failures;
    result.throughput.received_messages = total_received;
    result.throughput.received_bytes = total_received_bytes;
    result.throughput.messages_per_s = SafeDiv(static_cast<double>(total_received), wall_s);
    result.throughput.mb_per_s = SafeDiv(
        static_cast<double>(total_received_bytes), wall_s * 1024.0 * 1024.0);

    std::sort(all_latency_samples.begin(), all_latency_samples.end());
    result.latency.sample_count = all_latency_samples.size();
    result.latency.dropped_samples = total_dropped_samples;
    if (!all_latency_samples.empty()) {
      result.latency.min_ns = all_latency_samples.front();
      result.latency.max_ns = all_latency_samples.back();
      result.latency.p50_ns =
          static_cast<uint64_t>(PercentileFromSorted(all_latency_samples, 50.0));
      result.latency.p95_ns =
          static_cast<uint64_t>(PercentileFromSorted(all_latency_samples, 95.0));
      result.latency.p99_ns =
          static_cast<uint64_t>(PercentileFromSorted(all_latency_samples, 99.0));
      result.latency.p999_ns = static_cast<uint64_t>(
          PercentileFromSorted(all_latency_samples, 99.9));
    }

    const uint64_t expected_total =
        total_sent * static_cast<uint64_t>(config.subscribers);
    result.reliability.total_loss = total_loss;
    result.reliability.max_consecutive_loss = max_consecutive_loss;
    result.reliability.duplicate_or_reordered = duplicates;
    result.reliability.loss_rate =
        expected_total == 0
            ? 0.0
            : static_cast<double>(total_loss) / static_cast<double>(expected_total);

    result.resource.cpu_utilization_percent = SafeDiv(cpu_delta_total, wall_s) * 100.0;
    result.resource.cpu_cost_us_per_message = SafeDiv(
        cpu_delta_total * 1e6, static_cast<double>(std::max<uint64_t>(1, total_received)));
    result.resource.rss_kb_begin = rss_begin_sum;
    result.resource.rss_kb_end = rss_end_sum;
    result.resource.rss_kb_peak_observed = std::max(rss_begin_sum, rss_end_sum);
    result.resource.voluntary_context_switches = voluntary_ctx_sum;
    result.resource.involuntary_context_switches = involuntary_ctx_sum;
    result.resource.context_switches = voluntary_ctx_sum + involuntary_ctx_sum;

    result.shm_loan_supported = any_loan_supported;
    result.shm_profile_recorded = total_borrowed > 0 && total_copies == 0;
    std::ostringstream notes;
    notes << "real_multi_process=true"
          << " | worker_cpu_set=" << cpu_set_text
          << " | transport_mode=" << worker_transport_mode
          << " | pub_transport_mode_seen=" << pub_transport_mode_seen
          << " | sub_transport_mode_seen=" << sub_transport_mode_seen
          << " | loan_supported="
          << (result.shm_loan_supported ? "true" : "false");
    if (config.coverage == CoverageMode::kInterHost) {
      notes << " | inter_host_uses_separate_processes_on_single_machine";
    }
    if (config.coverage == CoverageMode::kInterProcess) {
      notes << " | zero_copy_borrowed_messages=" << total_borrowed
            << " | zero_copy_copy_count=" << total_copies;
    }
    result.notes = notes.str();
    result.success = true;

    cleanup_case_files();
    return result;
  }

  BenchmarkCaseResult RunSingleCaseInProcess(const BenchmarkCaseConfig& config) {
    BenchmarkCaseResult result;
    result.config = config;
    if (config.message_type == MessageType::kPod) {
      result.success = false;
      result.error_message = "in-process runner does not support pod message_type";
      return result;
    }

    RuntimeCounters runtime;
    runtime.latency_samples.resize(options_.latency_sample_cap, 0);
    runtime.sequence_trackers.assign(
        static_cast<size_t>(config.subscribers),
        std::vector<SequenceTracker>(static_cast<size_t>(config.publishers)));
    runtime.sequence_locks.reserve(static_cast<size_t>(config.subscribers));
    for (int i = 0; i < config.subscribers; ++i) {
      runtime.sequence_locks.emplace_back(std::make_unique<std::mutex>());
    }

    const auto mode = ToTransportMode(config.coverage);
    const uint64_t case_suffix = ++case_counter_;
    const std::string channel_name = MakeChannelName(config, case_suffix);
    const int process_id = common::GlobalData::Instance()->ProcessId();
    const std::string host_ip = common::GlobalData::Instance()->HostIp();
    const int receiver_pid_base = process_id + 1000;

    std::vector<std::shared_ptr<transport::Receiver<apollo::cyber::proto::Chatter>>>
        receivers;
    receivers.reserve(static_cast<size_t>(config.subscribers));

    for (int receiver_index = 0; receiver_index < config.subscribers;
         ++receiver_index) {
      const int receiver_pid =
          ShouldUseDifferentProcess(config.coverage)
              ? receiver_pid_base + receiver_index
              : process_id;
      const std::string receiver_host =
          ReceiverHostIpForCoverage(config.coverage, receiver_index);
      const auto attr = BuildRoleAttributes(
          channel_name, "perf_sub_" + std::to_string(receiver_index),
          receiver_host, receiver_pid, case_suffix * 101 + receiver_index + 1);
      auto receiver = transport::Transport::Instance()->CreateReceiver<
          apollo::cyber::proto::Chatter>(
          attr,
          [&, receiver_index](
              const std::shared_ptr<apollo::cyber::proto::Chatter>& msg,
              const transport::MessageInfo&,
              const apollo::cyber::proto::RoleAttributes&) {
            if (msg == nullptr) {
              return;
            }
            const uint64_t now_ns = MonotonicRawNowNs();
            const uint64_t sent_ns = msg->timestamp();
            const uint64_t latency_ns = now_ns >= sent_ns ? now_ns - sent_ns : 0;
            const uint64_t sample_idx =
                runtime.sample_count.fetch_add(1, std::memory_order_relaxed);
            if (sample_idx < runtime.latency_samples.size()) {
              runtime.latency_samples[static_cast<size_t>(sample_idx)] = latency_ns;
            } else {
              runtime.dropped_samples.fetch_add(1, std::memory_order_relaxed);
            }
            runtime.received_messages.fetch_add(1, std::memory_order_relaxed);
            runtime.received_bytes.fetch_add(
                static_cast<uint64_t>(msg->content().size()),
                std::memory_order_relaxed);

            const uint64_t publisher_id = msg->lidar_timestamp();
            if (publisher_id >= static_cast<uint64_t>(config.publishers)) {
              return;
            }
            std::lock_guard<std::mutex> lock(
                *runtime.sequence_locks[static_cast<size_t>(receiver_index)]);
            auto& tracker = runtime.sequence_trackers
                [static_cast<size_t>(receiver_index)][static_cast<size_t>(publisher_id)];
            const uint64_t seq = msg->seq();
            if (!tracker.initialized) {
              tracker.initialized = true;
              if (seq > 0) {
                tracker.internal_loss += seq;
                tracker.max_consecutive_loss =
                    std::max(tracker.max_consecutive_loss, seq);
              }
              tracker.expected_seq = seq + 1;
              tracker.received_unique += 1;
              return;
            }
            if (seq == tracker.expected_seq) {
              tracker.expected_seq += 1;
              tracker.received_unique += 1;
              return;
            }
            if (seq > tracker.expected_seq) {
              const uint64_t gap = seq - tracker.expected_seq;
              tracker.internal_loss += gap;
              tracker.max_consecutive_loss =
                  std::max(tracker.max_consecutive_loss, gap);
              tracker.expected_seq = seq + 1;
              tracker.received_unique += 1;
              return;
            }
            tracker.duplicate_or_reordered += 1;
          },
          mode);
      if (receiver == nullptr) {
        result.success = false;
        result.error_message = "failed to create receiver";
        return result;
      }
      receivers.emplace_back(std::move(receiver));
    }

    std::vector<std::shared_ptr<transport::Transmitter<apollo::cyber::proto::Chatter>>>
        transmitters;
    transmitters.reserve(static_cast<size_t>(config.publishers));
    for (int publisher_index = 0; publisher_index < config.publishers;
         ++publisher_index) {
      const auto attr = BuildRoleAttributes(
          channel_name, "perf_pub_" + std::to_string(publisher_index), host_ip,
          process_id, case_suffix * 131 + publisher_index + 1);
      auto transmitter = transport::Transport::Instance()->CreateTransmitter<
          apollo::cyber::proto::Chatter>(attr, mode);
      if (transmitter == nullptr) {
        result.success = false;
        result.error_message = "failed to create transmitter";
        for (auto& receiver : receivers) {
          receiver->Disable();
        }
        return result;
      }
      transmitters.emplace_back(std::move(transmitter));
    }

    const int pool_depth = ComputeMessagePoolDepth(options_, config);
    result.message_pool_depth = pool_depth;
    std::vector<PublisherMessagePool> pools(static_cast<size_t>(config.publishers));
    for (int publisher_index = 0; publisher_index < config.publishers;
         ++publisher_index) {
      auto& pool = pools[static_cast<size_t>(publisher_index)];
      pool.messages.reserve(static_cast<size_t>(pool_depth));
      std::string payload(static_cast<size_t>(config.payload_bytes), '\0');
      for (int i = 0; i < config.payload_bytes; ++i) {
        payload[static_cast<size_t>(i)] =
            static_cast<char>((publisher_index * 31 + i) % 251 + 1);
      }
      for (int slot = 0; slot < pool_depth; ++slot) {
        auto msg = std::make_shared<apollo::cyber::proto::Chatter>();
        msg->set_content(payload);
        msg->set_lidar_timestamp(static_cast<uint64_t>(publisher_index));
        msg->set_seq(0);
        msg->set_timestamp(0);
        pool.messages.emplace_back(std::move(msg));
      }
    }

    SleepNs(static_cast<uint64_t>(options_.startup_wait_ms) * kOneMillisecondNs);

    std::vector<std::thread> sender_threads;
    sender_threads.reserve(static_cast<size_t>(config.publishers));
    std::vector<std::atomic<uint64_t>> sent_counts(
        static_cast<size_t>(config.publishers));
    std::vector<std::atomic<uint64_t>> send_failures(
        static_cast<size_t>(config.publishers));
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};

    CpuInterferenceController interference;
    if (config.cpu_interference_percent > 0) {
      StartCpuInterference(&interference, config.cpu_interference_percent,
                           options_.cpu_set);
    }

    for (int publisher_index = 0; publisher_index < config.publishers;
         ++publisher_index) {
      sent_counts[static_cast<size_t>(publisher_index)].store(0);
      send_failures[static_cast<size_t>(publisher_index)].store(0);
      sender_threads.emplace_back([&, publisher_index]() {
        const int cpu = options_.cpu_set[static_cast<size_t>(
            publisher_index % static_cast<int>(options_.cpu_set.size()))];
        (void)PinCurrentThreadToCpu(cpu);
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        const uint64_t period_ns =
            config.frequency_hz > 0
                ? static_cast<uint64_t>(
                      std::max(1, static_cast<int>(kOneSecondNs / config.frequency_hz)))
                : 0;
        uint64_t next_send_ns = MonotonicRawNowNs();
        uint64_t seq = 0;
        auto& pool = pools[static_cast<size_t>(publisher_index)];
        const size_t depth = pool.messages.size();
        while (!stop.load(std::memory_order_acquire)) {
          const uint64_t now = MonotonicRawNowNs();
          if (period_ns > 0 && now < next_send_ns) {
            SleepUntilNs(next_send_ns);
            continue;
          }
          auto& msg = pool.messages[static_cast<size_t>(seq % depth)];
          msg->set_seq(seq);
          msg->set_timestamp(now);
          msg->set_lidar_timestamp(static_cast<uint64_t>(publisher_index));
          if (transmitters[static_cast<size_t>(publisher_index)]->Transmit(msg)) {
            sent_counts[static_cast<size_t>(publisher_index)].fetch_add(
                1, std::memory_order_relaxed);
          } else {
            send_failures[static_cast<size_t>(publisher_index)].fetch_add(
                1, std::memory_order_relaxed);
          }
          ++seq;
          if (period_ns > 0) {
            next_send_ns += period_ns;
          }
        }
      });
    }

    const ResourceSnapshot begin = CaptureResourceSnapshot();
    go.store(true, std::memory_order_release);
    const uint64_t run_start_ns = MonotonicRawNowNs();
    SleepNs(static_cast<uint64_t>(config.duration_s) * kOneSecondNs);
    stop.store(true, std::memory_order_release);

    for (auto& sender : sender_threads) {
      if (sender.joinable()) {
        sender.join();
      }
    }
    StopCpuInterference(&interference);

    SleepNs(static_cast<uint64_t>(options_.cooldown_wait_ms) * kOneMillisecondNs);
    for (int i = 0; i < 20; ++i) {
      const uint64_t before =
          runtime.received_messages.load(std::memory_order_relaxed);
      SleepNs(50000000ULL);  // 50ms
      const uint64_t after =
          runtime.received_messages.load(std::memory_order_relaxed);
      if (before == after) {
        break;
      }
    }
    const ResourceSnapshot end = CaptureResourceSnapshot();
    const uint64_t run_end_ns = MonotonicRawNowNs();
    result.wall_time_ns =
        run_end_ns > run_start_ns ? run_end_ns - run_start_ns : 0;

    for (auto& transmitter : transmitters) {
      transmitter->Disable();
    }
    for (auto& receiver : receivers) {
      receiver->Disable();
    }

    const uint64_t sampled = runtime.sample_count.load(std::memory_order_relaxed);
    const uint64_t kept =
        std::min<uint64_t>(sampled, static_cast<uint64_t>(runtime.latency_samples.size()));
    result.latency.sample_count = kept;
    result.latency.dropped_samples =
        runtime.dropped_samples.load(std::memory_order_relaxed);
    if (kept > 0) {
      std::vector<uint64_t> sorted;
      sorted.reserve(static_cast<size_t>(kept));
      for (uint64_t i = 0; i < kept; ++i) {
        sorted.push_back(runtime.latency_samples[static_cast<size_t>(i)]);
      }
      std::sort(sorted.begin(), sorted.end());
      result.latency.min_ns = sorted.front();
      result.latency.max_ns = sorted.back();
      result.latency.p50_ns = static_cast<uint64_t>(PercentileFromSorted(sorted, 50.0));
      result.latency.p95_ns = static_cast<uint64_t>(PercentileFromSorted(sorted, 95.0));
      result.latency.p99_ns = static_cast<uint64_t>(PercentileFromSorted(sorted, 99.0));
      result.latency.p999_ns =
          static_cast<uint64_t>(PercentileFromSorted(sorted, 99.9));
    }

    uint64_t total_sent = 0;
    uint64_t total_send_failures = 0;
    for (int i = 0; i < config.publishers; ++i) {
      total_sent += sent_counts[static_cast<size_t>(i)].load(std::memory_order_relaxed);
      total_send_failures +=
          send_failures[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    }
    const uint64_t total_received =
        runtime.received_messages.load(std::memory_order_relaxed);
    const uint64_t total_received_bytes =
        runtime.received_bytes.load(std::memory_order_relaxed);
    const double wall_s = SafeDiv(static_cast<double>(result.wall_time_ns), 1e9);
    result.throughput.sent_messages = total_sent;
    result.throughput.send_failures = total_send_failures;
    result.throughput.received_messages = total_received;
    result.throughput.received_bytes = total_received_bytes;
    result.throughput.messages_per_s =
        SafeDiv(static_cast<double>(total_received), wall_s);
    result.throughput.mb_per_s =
        SafeDiv(static_cast<double>(total_received_bytes), wall_s * 1024.0 * 1024.0);

    uint64_t total_loss = 0;
    uint64_t max_consecutive_loss = 0;
    uint64_t duplicates = 0;
    for (int receiver_index = 0; receiver_index < config.subscribers;
         ++receiver_index) {
      std::lock_guard<std::mutex> lock(
          *runtime.sequence_locks[static_cast<size_t>(receiver_index)]);
      for (int publisher_index = 0; publisher_index < config.publishers;
           ++publisher_index) {
        const auto sent_count =
            sent_counts[static_cast<size_t>(publisher_index)].load(std::memory_order_relaxed);
        const auto& tracker = runtime.sequence_trackers
            [static_cast<size_t>(receiver_index)][static_cast<size_t>(publisher_index)];
        uint64_t tail_loss = 0;
        if (!tracker.initialized) {
          tail_loss = sent_count;
        } else if (sent_count > tracker.expected_seq) {
          tail_loss = sent_count - tracker.expected_seq;
        }
        total_loss += tracker.internal_loss + tail_loss;
        max_consecutive_loss =
            std::max(max_consecutive_loss,
                     std::max(tracker.max_consecutive_loss, tail_loss));
        duplicates += tracker.duplicate_or_reordered;
      }
    }
    const uint64_t expected_total =
        total_sent * static_cast<uint64_t>(config.subscribers);
    result.reliability.total_loss = total_loss;
    result.reliability.max_consecutive_loss = max_consecutive_loss;
    result.reliability.duplicate_or_reordered = duplicates;
    result.reliability.loss_rate =
        expected_total == 0
            ? 0.0
            : static_cast<double>(total_loss) / static_cast<double>(expected_total);

    const double cpu_begin = begin.cpu_user_s + begin.cpu_sys_s;
    const double cpu_end = end.cpu_user_s + end.cpu_sys_s;
    const double cpu_delta = std::max(0.0, cpu_end - cpu_begin);
    result.resource.cpu_utilization_percent = SafeDiv(cpu_delta, wall_s) * 100.0;
    result.resource.cpu_cost_us_per_message =
        SafeDiv(cpu_delta * 1e6, static_cast<double>(std::max<uint64_t>(1, total_received)));
    result.resource.rss_kb_begin = begin.rss_kb;
    result.resource.rss_kb_end = end.rss_kb;
    result.resource.rss_kb_peak_observed =
        std::max(begin.rss_kb, end.rss_kb);
    result.resource.voluntary_context_switches =
        end.voluntary_ctx_switches - begin.voluntary_ctx_switches;
    result.resource.involuntary_context_switches =
        end.involuntary_ctx_switches - begin.involuntary_ctx_switches;
    result.resource.context_switches =
        result.resource.voluntary_context_switches +
        result.resource.involuntary_context_switches;

    result.shm_loan_supported = false;
    if (mode == apollo::cyber::proto::OptionalMode::SHM) {
      const auto toml = transport::TransportProfileRecorder::Instance()->GenerateToml();
      result.shm_profile_recorded =
          toml.find("name = \"" + channel_name + "\"") != std::string::npos;
      result.notes = "SHM profile recorded=" +
                     std::string(result.shm_profile_recorded ? "true" : "false");
    } else if (mode == apollo::cyber::proto::OptionalMode::RTPS &&
               config.coverage == CoverageMode::kInterHost) {
      result.notes = "inter_host mode is simulated with synthetic host_ip on one machine";
    }

    result.success = true;
    return result;
  }

  std::vector<CoverageMode> AllCoverages() const {
    return {CoverageMode::kIntraProcess, CoverageMode::kInterProcess,
            CoverageMode::kInterHost};
  }

  bool ExportResults() const {
    std::ofstream out(options_.output_path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
      return false;
    }
    out << "{\n";
    out << "  \"suite\": \"Cyber RT Benchmark Suite\",\n";
    out << "  \"host\": \"" << JsonEscape(common::GlobalData::Instance()->HostName())
        << "\",\n";
    out << "  \"host_ip\": \"" << JsonEscape(common::GlobalData::Instance()->HostIp())
        << "\",\n";
    out << "  \"result_count\": " << results_.size() << ",\n";
    out << "  \"results\": [\n";
    for (size_t i = 0; i < results_.size(); ++i) {
      const auto& result = results_[i];
      out << "    {\n";
      out << "      \"scenario\": \"" << JsonEscape(ToString(result.config.scenario))
          << "\",\n";
      out << "      \"coverage\": \"" << JsonEscape(ToString(result.config.coverage))
          << "\",\n";
      out << "      \"message_type\": \""
          << JsonEscape(ToString(result.config.message_type)) << "\",\n";
      out << "      \"topology\": \"" << JsonEscape(ToString(result.config.topology))
          << "\",\n";
      out << "      \"publishers\": " << result.config.publishers << ",\n";
      out << "      \"subscribers\": " << result.config.subscribers << ",\n";
      out << "      \"frequency_hz\": " << result.config.frequency_hz << ",\n";
      out << "      \"payload_bytes\": " << result.config.payload_bytes << ",\n";
      out << "      \"duration_s\": " << result.config.duration_s << ",\n";
      out << "      \"cpu_interference_percent\": "
          << result.config.cpu_interference_percent << ",\n";
      out << "      \"success\": " << (result.success ? "true" : "false")
          << ",\n";
      out << "      \"error_message\": \"" << JsonEscape(result.error_message)
          << "\",\n";
      out << "      \"notes\": \"" << JsonEscape(result.notes) << "\",\n";
      out << "      \"wall_time_ns\": " << result.wall_time_ns << ",\n";
      out << "      \"message_pool_depth\": " << result.message_pool_depth << ",\n";
      out << "      \"shm_profile_recorded\": "
          << (result.shm_profile_recorded ? "true" : "false") << ",\n";
      out << "      \"shm_loan_supported\": "
          << (result.shm_loan_supported ? "true" : "false") << ",\n";

      out << "      \"latency\": {\n";
      out << "        \"min_ns\": " << result.latency.min_ns << ",\n";
      out << "        \"p50_ns\": " << result.latency.p50_ns << ",\n";
      out << "        \"p95_ns\": " << result.latency.p95_ns << ",\n";
      out << "        \"p99_ns\": " << result.latency.p99_ns << ",\n";
      out << "        \"p99_9_ns\": " << result.latency.p999_ns << ",\n";
      out << "        \"max_ns\": " << result.latency.max_ns << ",\n";
      out << "        \"sample_count\": " << result.latency.sample_count << ",\n";
      out << "        \"dropped_samples\": " << result.latency.dropped_samples
          << "\n";
      out << "      },\n";

      out << "      \"throughput\": {\n";
      out << "        \"messages_per_s\": " << result.throughput.messages_per_s
          << ",\n";
      out << "        \"mb_per_s\": " << result.throughput.mb_per_s << ",\n";
      out << "        \"sent_messages\": " << result.throughput.sent_messages
          << ",\n";
      out << "        \"send_failures\": " << result.throughput.send_failures
          << ",\n";
      out << "        \"received_messages\": "
          << result.throughput.received_messages << ",\n";
      out << "        \"received_bytes\": " << result.throughput.received_bytes
          << "\n";
      out << "      },\n";

      out << "      \"reliability\": {\n";
      out << "        \"loss_rate\": " << result.reliability.loss_rate << ",\n";
      out << "        \"total_loss\": " << result.reliability.total_loss << ",\n";
      out << "        \"max_consecutive_loss\": "
          << result.reliability.max_consecutive_loss << ",\n";
      out << "        \"duplicate_or_reordered\": "
          << result.reliability.duplicate_or_reordered << "\n";
      out << "      },\n";

      out << "      \"resource\": {\n";
      out << "        \"cpu_cost_us_per_message\": "
          << result.resource.cpu_cost_us_per_message << ",\n";
      out << "        \"cpu_utilization_percent\": "
          << result.resource.cpu_utilization_percent << ",\n";
      out << "        \"rss_kb_begin\": " << result.resource.rss_kb_begin << ",\n";
      out << "        \"rss_kb_end\": " << result.resource.rss_kb_end << ",\n";
      out << "        \"rss_kb_peak_observed\": "
          << result.resource.rss_kb_peak_observed << ",\n";
      out << "        \"context_switches\": " << result.resource.context_switches
          << ",\n";
      out << "        \"voluntary_context_switches\": "
          << result.resource.voluntary_context_switches << ",\n";
      out << "        \"involuntary_context_switches\": "
          << result.resource.involuntary_context_switches << "\n";
      out << "      }\n";

      out << "    }" << (i + 1 < results_.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    out.flush();
    if (out.fail()) {
      return false;
    }
    out.close();
    std::ifstream verify(options_.output_path);
    return verify.good();
  }

  BenchmarkOptions options_;
  std::vector<BenchmarkCaseResult> results_;
  uint64_t case_counter_ = 0;
};

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

  apollo::cyber::examples::perf_test::BenchmarkOptions options;
  std::string parse_error;
  if (!apollo::cyber::examples::perf_test::ParseOptions(argc, argv, &options,
                                                        &parse_error)) {
    apollo::cyber::examples::perf_test::PrintUsage();
    if (!parse_error.empty()) {
      std::cerr << "argument parse error: " << parse_error << std::endl;
      return 2;
    }
    return 0;
  }

  const std::string bin_dir =
      apollo::cyber::examples::perf_test::DirName(argv[0]);
  if (options.benchmark_pub_binary.empty()) {
    options.benchmark_pub_binary =
        apollo::cyber::examples::perf_test::JoinPath(bin_dir, "benchmark_pub");
  }
  if (options.benchmark_sub_binary.empty()) {
    options.benchmark_sub_binary =
        apollo::cyber::examples::perf_test::JoinPath(bin_dir, "benchmark_sub");
  }

  if (!options.cpu_set.empty()) {
    (void)apollo::cyber::examples::perf_test::PinCurrentThreadToCpu(
        options.cpu_set.front());
  }

  apollo::cyber::Init(argv[0]);
  apollo::cyber::transport::Transport::Instance();

  apollo::cyber::examples::perf_test::BenchmarkSuiteRunner runner(
      std::move(options));
  const bool ok = runner.Run();

  size_t success_count = 0;
  size_t fail_count = 0;
  for (const auto& result : runner.results()) {
    if (result.success) {
      ++success_count;
    } else {
      ++fail_count;
    }
  }
  std::cout << "Cyber RT Benchmark Suite completed: success=" << success_count
            << " fail=" << fail_count << std::endl;
  apollo::cyber::Clear();
  return ok ? 0 : 1;
}

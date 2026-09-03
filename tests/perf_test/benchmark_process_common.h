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

#ifndef CYBER_EXAMPLES_PERF_TEST_BENCHMARK_PROCESS_COMMON_H_
#define CYBER_EXAMPLES_PERF_TEST_BENCHMARK_PROCESS_COMMON_H_

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/common/global_data.h"
#include "cyber/proto/role_attributes.pb.h"
#include "cyber/transport/qos/qos_profile_conf.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

constexpr uint64_t kOneSecondNs = 1000000000ULL;
constexpr uint64_t kOneMillisecondNs = 1000000ULL;
constexpr uint64_t kBenchmarkWarmupSeqBase =
    std::numeric_limits<uint64_t>::max() - 1024;
constexpr uint64_t kDefaultSequenceCapacity = 1ULL << 20;

enum class CoverageMode {
  kIntraProcess,
  kInterProcess,
  kInterHost,
};

enum class MessageType {
  kProtobuf,
  kPod,
};

inline std::string ToString(CoverageMode mode) {
  switch (mode) {
    case CoverageMode::kIntraProcess:
      return "intra_process";
    case CoverageMode::kInterProcess:
      return "inter_process";
    case CoverageMode::kInterHost:
      return "inter_host";
  }
  return "intra_process";
}

inline bool ParseCoverageMode(const std::string& value, CoverageMode* mode) {
  if (mode == nullptr) {
    return false;
  }
  if (value == "intra_process") {
    *mode = CoverageMode::kIntraProcess;
    return true;
  }
  if (value == "inter_process") {
    *mode = CoverageMode::kInterProcess;
    return true;
  }
  if (value == "inter_host") {
    *mode = CoverageMode::kInterHost;
    return true;
  }
  return false;
}

inline std::string ToString(MessageType type) {
  switch (type) {
    case MessageType::kProtobuf:
      return "protobuf";
    case MessageType::kPod:
      return "pod";
  }
  return "protobuf";
}

inline bool ParseMessageType(const std::string& value, MessageType* type) {
  if (type == nullptr) {
    return false;
  }
  if (value == "protobuf") {
    *type = MessageType::kProtobuf;
    return true;
  }
  if (value == "pod") {
    *type = MessageType::kPod;
    return true;
  }
  return false;
}

inline apollo::cyber::proto::OptionalMode ToTransportMode(CoverageMode coverage) {
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

inline std::string ToString(apollo::cyber::proto::OptionalMode mode) {
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

inline bool ParseOptionalMode(
    const std::string& value, apollo::cyber::proto::OptionalMode* mode) {
  if (mode == nullptr) {
    return false;
  }
  if (value == "intra") {
    *mode = apollo::cyber::proto::OptionalMode::INTRA;
    return true;
  }
  if (value == "shm") {
    *mode = apollo::cyber::proto::OptionalMode::SHM;
    return true;
  }
  if (value == "rtps") {
    *mode = apollo::cyber::proto::OptionalMode::RTPS;
    return true;
  }
  if (value == "hybrid") {
    *mode = apollo::cyber::proto::OptionalMode::HYBRID;
    return true;
  }
  if (value == "iceoryx") {
    *mode = apollo::cyber::proto::OptionalMode::ICEORYX;
    return true;
  }
  return false;
}

inline uint64_t MonotonicRawNowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * kOneSecondNs +
         static_cast<uint64_t>(ts.tv_nsec);
}

inline void SleepNs(uint64_t duration_ns) {
  if (duration_ns == 0) {
    return;
  }
  timespec req;
  req.tv_sec = static_cast<time_t>(duration_ns / kOneSecondNs);
  req.tv_nsec = static_cast<long>(duration_ns % kOneSecondNs);
  while (nanosleep(&req, &req) != 0 && errno == EINTR) {
  }
}

inline void SleepUntilNs(uint64_t target_ns) {
  while (true) {
    const uint64_t now = MonotonicRawNowNs();
    if (now >= target_ns) {
      return;
    }
    const uint64_t remain = target_ns - now;
    if (remain > 50000ULL) {
      SleepNs(remain - 20000ULL);
    } else {
      sched_yield();
    }
  }
}

inline bool PinCurrentThreadToCpu(int cpu) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
}

inline int ParseIntOr(const std::string& value, int fallback) {
  if (value.empty()) {
    return fallback;
  }
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    return fallback;
  }
  return static_cast<int>(parsed);
}

inline int64_t ParseInt64Or(const std::string& value, int64_t fallback) {
  if (value.empty()) {
    return fallback;
  }
  char* end = nullptr;
  errno = 0;
  const long long parsed = std::strtoll(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    return fallback;
  }
  return static_cast<int64_t>(parsed);
}

inline uint64_t ParseUInt64Or(const std::string& value, uint64_t fallback) {
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

inline double ParseDoubleOr(const std::string& value, double fallback) {
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

inline bool ParseBoolOr(const std::string& value, bool fallback) {
  if (value == "1" || value == "true" || value == "TRUE" || value == "True") {
    return true;
  }
  if (value == "0" || value == "false" || value == "FALSE" ||
      value == "False") {
    return false;
  }
  return fallback;
}

inline std::vector<int> ParseCpuSet(const std::string& value) {
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
  return cpus;
}

inline bool ParseArgMap(int argc, char** argv,
                        std::unordered_map<std::string, std::string>* out,
                        std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "null arg map";
    }
    return false;
  }
  out->clear();
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    const auto pos = arg.find('=');
    if (pos == std::string::npos) {
      if (arg.rfind("--", 0) == 0) {
        (*out)[arg] = "true";
        continue;
      }
      if (error != nullptr) {
        *error = "invalid argument: " + arg;
      }
      return false;
    }
    const std::string key = arg.substr(0, pos);
    const std::string value = arg.substr(pos + 1);
    (*out)[key] = value;
  }
  return true;
}

inline std::string GetArgOr(const std::unordered_map<std::string, std::string>& args,
                            const std::string& key,
                            const std::string& fallback) {
  const auto it = args.find(key);
  if (it == args.end()) {
    return fallback;
  }
  return it->second;
}

inline bool WriteKvFile(const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& kvs) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  for (const auto& kv : kvs) {
    out << kv.first << "=" << kv.second << "\n";
  }
  out.flush();
  return !out.fail();
}

inline bool AppendKvFile(
    const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& kvs) {
  std::ofstream out(path, std::ios::out | std::ios::app);
  if (!out.is_open()) {
    return false;
  }
  for (const auto& kv : kvs) {
    out << kv.first << "=" << kv.second << "\n";
  }
  out.flush();
  return !out.fail();
}

inline bool ReadKvFile(const std::string& path,
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

inline bool ReadMeasurementStart(const std::string& path,
                                 uint64_t* measurement_start_ns) {
  if (path.empty() || measurement_start_ns == nullptr) {
    return false;
  }
  std::unordered_map<std::string, std::string> kv;
  if (!ReadKvFile(path, &kv)) {
    return false;
  }
  const auto it = kv.find("measurement_start_ns");
  if (it == kv.end()) {
    return false;
  }
  const uint64_t value = ParseUInt64Or(it->second, 0);
  if (value == 0) {
    return false;
  }
  *measurement_start_ns = value;
  return true;
}

inline std::string JoinCpuSet(const std::vector<int>& cpus) {
  std::ostringstream oss;
  for (size_t i = 0; i < cpus.size(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << cpus[i];
  }
  return oss.str();
}

inline apollo::cyber::proto::RoleAttributes BuildRoleAttributes(
    const std::string& channel_name, const std::string& node_name,
    const std::string& host_ip, int process_id, uint64_t unique_seed,
    int qos_depth = 4096) {
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
  attr.mutable_qos_profile()->set_depth(std::max(1, qos_depth));
  return attr;
}

inline long ReadVmRssKb() {
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

inline ResourceSnapshot CaptureResourceSnapshot() {
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

inline double SafeDiv(double a, double b) {
  if (std::abs(b) < std::numeric_limits<double>::epsilon()) {
    return 0.0;
  }
  return a / b;
}

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_EXAMPLES_PERF_TEST_BENCHMARK_PROCESS_COMMON_H_

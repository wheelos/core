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
#include <fcntl.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstring>
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

#include "tests/perf_test/benchmark_measurement_window.h"
#include "tests/perf_test/benchmark_rate_acceptance.h"
#include "tests/perf_test/benchmark_sequence_tracker.h"
#include "tests/perf_test/fanout_validation.h"
#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/init.h"
#include "cyber/proto/unit_test.pb.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/qos/qos_profile_conf.h"
#include "cyber/transport/iceoryx_chunk.h"
#include "cyber/transport/shm/profile.h"
#include "cyber/transport/transport.h"

extern char** environ;

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

namespace {

constexpr uint64_t kOneSecondNs = 1000000000ULL;
constexpr uint64_t kOneMillisecondNs = 1000000ULL;
constexpr uint64_t kOneMegabyte = 1024ULL * 1024ULL;
constexpr uint64_t kBenchmarkWarmupSeqBase =
    std::numeric_limits<uint64_t>::max() - 1024;

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

std::vector<int> ParsePositiveIntList(const std::string& value) {
  std::vector<int> values;
  std::stringstream ss(value);
  std::string token;
  while (std::getline(ss, token, ',')) {
    const int parsed = ParseIntOr(token, 0);
    if (parsed > 0) {
      values.push_back(parsed);
    }
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
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

bool WriteKvFile(
    const std::string& path,
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
  std::string stdout_path;
  std::string stderr_path;
  std::string command;
  bool exited = false;
  int exit_code = -1;
  int term_signal = 0;
};

using EnvironmentOverrides =
    std::vector<std::pair<std::string, std::string>>;

std::string JoinCommand(const std::vector<std::string>& args) {
  std::ostringstream command;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      command << " ";
    }
    command << args[i];
  }
  return command.str();
}

bool SpawnProcess(const std::vector<std::string>& args, ChildProcess* child,
                  std::string* error,
                  const EnvironmentOverrides& environment = {}) {
  if (child == nullptr || args.empty()) {
    if (error != nullptr) {
      *error = "invalid spawn parameters";
    }
    return false;
  }
  std::unordered_map<std::string, std::string> remaining_environment;
  for (const auto& variable : environment) {
    remaining_environment[variable.first] = variable.second;
  }
  std::vector<std::string> environment_storage;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string current(*entry);
    const auto separator = current.find('=');
    const std::string key = current.substr(0, separator);
    const auto override_it = remaining_environment.find(key);
    if (override_it == remaining_environment.end()) {
      environment_storage.push_back(current);
    } else {
      environment_storage.push_back(key + "=" + override_it->second);
      remaining_environment.erase(override_it);
    }
  }
  for (const auto& variable : remaining_environment) {
    environment_storage.push_back(variable.first + "=" + variable.second);
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  std::vector<char*> envp;
  envp.reserve(environment_storage.size() + 1);
  for (auto& variable : environment_storage) {
    envp.push_back(variable.data());
  }
  envp.push_back(nullptr);

  posix_spawn_file_actions_t file_actions;
  int spawn_error = posix_spawn_file_actions_init(&file_actions);
  const bool file_actions_initialized = spawn_error == 0;
  if (spawn_error == 0) {
    spawn_error = posix_spawn_file_actions_addopen(
        &file_actions, STDOUT_FILENO, child->stdout_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC, 0644);
  }
  if (spawn_error == 0) {
    spawn_error = posix_spawn_file_actions_addopen(
        &file_actions, STDERR_FILENO, child->stderr_path.c_str(),
        O_CREAT | O_WRONLY | O_TRUNC, 0644);
  }
  pid_t pid = -1;
  if (spawn_error == 0) {
    spawn_error = posix_spawn(&pid, args[0].c_str(), &file_actions, nullptr,
                              argv.data(), envp.data());
  }
  if (file_actions_initialized) {
    (void)posix_spawn_file_actions_destroy(&file_actions);
  }
  if (spawn_error != 0) {
    if (error != nullptr) {
      *error = "posix_spawn failed: " +
               std::string(std::strerror(spawn_error));
    }
    return false;
  }
  child->pid = pid;
  std::ostringstream command;
  for (const auto& variable : environment) {
    command << variable.first << "=" << variable.second << " ";
  }
  command << JoinCommand(args);
  child->command = command.str();
  return true;
}

void RecordChildExitStatus(int status, ChildProcess* child) {
  child->exited = true;
  if (WIFEXITED(status)) {
    child->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    child->term_signal = WTERMSIG(status);
  }
}

bool KillAndReapChildren(std::vector<ChildProcess>* children,
                         int timeout_ms, std::string* error = nullptr) {
  if (children == nullptr) {
    return true;
  }
  for (const auto& child : *children) {
    if (child.pid > 0 && !child.exited) {
      (void)kill(child.pid, SIGKILL);
    }
  }
  const uint64_t deadline =
      MonotonicRawNowNs() +
      static_cast<uint64_t>(std::max(1, timeout_ms)) * kOneMillisecondNs;
  while (MonotonicRawNowNs() < deadline) {
    bool pending = false;
    for (auto& child : *children) {
      if (child.pid <= 0 || child.exited) {
        continue;
      }
      int status = 0;
      const pid_t waited = waitpid(child.pid, &status, WNOHANG);
      if (waited == child.pid) {
        RecordChildExitStatus(status, &child);
      } else if (waited < 0 && errno == ECHILD) {
        child.exited = true;
      } else {
        pending = true;
      }
    }
    if (!pending) {
      return true;
    }
    SleepNs(10 * kOneMillisecondNs);
  }
  if (error != nullptr) {
    std::ostringstream description;
    description << "worker cleanup timeout";
    for (const auto& child : *children) {
      if (child.pid > 0 && !child.exited) {
        description << " role=" << child.role << " pid=" << child.pid;
      }
    }
    *error = description.str();
  }
  return false;
}

std::string ReadWorkerOutput(const std::string& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return "<unavailable>";
  }
  std::ostringstream output;
  output << input.rdbuf();
  std::string text = output.str();
  constexpr size_t kMaxDiagnosticBytes = 4096;
  if (text.size() > kMaxDiagnosticBytes) {
    text = "...<truncated>..." +
           text.substr(text.size() - kMaxDiagnosticBytes);
  }
  return text.empty() ? "<empty>" : text;
}

std::string DescribeChildFailure(const ChildProcess& child) {
  std::ostringstream description;
  description << "role=" << child.role << " pid=" << child.pid;
  if (child.term_signal != 0) {
    description << " signal=" << child.term_signal;
  } else {
    description << " exit=" << child.exit_code;
  }
  description << " command=[" << child.command << "]"
              << " stdout=[" << ReadWorkerOutput(child.stdout_path) << "]"
              << " stderr=[" << ReadWorkerOutput(child.stderr_path) << "]"
              << " result=[" << ReadWorkerOutput(child.result_path) << "]";
  return description.str();
}

class BenchmarkRouDiProcess {
 public:
  BenchmarkRouDiProcess(std::string binary, std::string output_dir,
                        uint64_t chunk_size, uint32_t chunk_count,
                        uint32_t publisher_history_capacity,
                        uint32_t in_flight_margin)
      : binary_(std::move(binary)),
        output_dir_(std::move(output_dir)),
        chunk_size_(chunk_size),
        chunk_count_(chunk_count),
        publisher_history_capacity_(publisher_history_capacity),
        in_flight_margin_(in_flight_margin) {}

  ~BenchmarkRouDiProcess() {
    if (running_) {
      (void)Stop();
    }
  }

  bool Start(std::string* error) {
    if (binary_.empty() || !FileExists(binary_)) {
      if (error != nullptr) {
        *error = "benchmark RouDi binary does not exist: " + binary_;
      }
      return false;
    }
    const std::string suffix =
        std::to_string(getpid()) + "_" + std::to_string(MonotonicRawNowNs());
    ready_path_ =
        JoinPath(output_dir_, ".cyber_rt_perf_roudi_" + suffix + ".ready");
    result_path_ =
        JoinPath(output_dir_, ".cyber_rt_perf_roudi_" + suffix + ".result");
    child_.role = "benchmark_roudi";
    child_.result_path = result_path_;
    child_.stdout_path =
        JoinPath(output_dir_, ".cyber_rt_perf_roudi_" + suffix + ".stdout");
    child_.stderr_path =
        JoinPath(output_dir_, ".cyber_rt_perf_roudi_" + suffix + ".stderr");
    const std::vector<std::string> args = {
        binary_,
        "--ready_path=" + ready_path_,
        "--result_path=" + result_path_,
        "--chunk_size=" + std::to_string(chunk_size_),
        "--chunk_count=" + std::to_string(chunk_count_),
    };
    if (!SpawnProcess(args, &child_, error)) {
      return false;
    }
    started_ = true;
    running_ = true;

    const uint64_t deadline = MonotonicRawNowNs() + 10 * kOneSecondNs;
    while (MonotonicRawNowNs() < deadline) {
      if (FileExists(ready_path_)) {
        return true;
      }
      if (!CheckRunning(error)) {
        return false;
      }
      SleepNs(10 * kOneMillisecondNs);
    }
    if (error != nullptr) {
      *error = "benchmark RouDi readiness timeout: " +
               DescribeChildFailure(child_);
    }
    (void)Stop();
    return false;
  }

  bool CheckRunning(std::string* error) {
    if (!running_) {
      if (error != nullptr) {
        *error = "benchmark RouDi is not running";
      }
      return false;
    }
    int status = 0;
    const pid_t waited = waitpid(child_.pid, &status, WNOHANG);
    if (waited == 0) {
      return true;
    }
    if (waited < 0) {
      if (errno == EINTR) {
        return true;
      }
      if (error != nullptr) {
        *error = "waitpid failed for benchmark RouDi pid=" +
                 std::to_string(child_.pid) +
                 " errno=" + std::to_string(errno);
      }
      return false;
    }
    RecordExitStatus(status);
    running_ = false;
    if (error != nullptr) {
      *error = "benchmark RouDi exited unexpectedly: " +
               DescribeChildFailure(child_);
    }
    return false;
  }

  bool Stop() {
    if (!started_) {
      return true;
    }
    if (!running_) {
      return clean_shutdown_;
    }
    if (kill(child_.pid, SIGTERM) != 0 && errno != ESRCH) {
      return false;
    }
    const uint64_t deadline = MonotonicRawNowNs() + 5 * kOneSecondNs;
    while (MonotonicRawNowNs() < deadline) {
      int status = 0;
      const pid_t waited = waitpid(child_.pid, &status, WNOHANG);
      if (waited == child_.pid) {
        RecordExitStatus(status);
        running_ = false;
        std::unordered_map<std::string, std::string> result;
        clean_shutdown_ =
            child_.exit_code == 0 && ReadKvFile(result_path_, &result) &&
            ParseIntOr(result["shutdown_complete"], 0) == 1;
        CleanupFiles(clean_shutdown_);
        return clean_shutdown_;
      }
      if (waited < 0 && errno != EINTR) {
        running_ = false;
        return false;
      }
      SleepNs(50 * kOneMillisecondNs);
    }
    if (kill(child_.pid, SIGKILL) == 0 || errno == ESRCH) {
      const uint64_t kill_deadline =
          MonotonicRawNowNs() + 1 * kOneSecondNs;
      while (MonotonicRawNowNs() < kill_deadline) {
        int status = 0;
        if (waitpid(child_.pid, &status, WNOHANG) == child_.pid) {
          RecordExitStatus(status);
          break;
        }
        SleepNs(10 * kOneMillisecondNs);
      }
    }
    running_ = false;
    clean_shutdown_ = false;
    return false;
  }

  bool started() const { return started_; }
  bool clean_shutdown() const { return clean_shutdown_; }
  pid_t pid() const { return child_.pid; }
  uint64_t chunk_size() const { return chunk_size_; }
  uint32_t chunk_count() const { return chunk_count_; }
  uint32_t publisher_history_capacity() const {
    return publisher_history_capacity_;
  }
  uint32_t in_flight_margin() const { return in_flight_margin_; }
  int exit_code() const { return child_.exit_code; }
  int term_signal() const { return child_.term_signal; }

 private:
  void RecordExitStatus(int status) {
    child_.exited = true;
    if (WIFEXITED(status)) {
      child_.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      child_.term_signal = WTERMSIG(status);
    }
  }

  void CleanupFiles(bool remove_logs) {
    RemoveFileIfExists(ready_path_);
    RemoveFileIfExists(result_path_);
    if (remove_logs) {
      RemoveFileIfExists(child_.stdout_path);
      RemoveFileIfExists(child_.stderr_path);
    }
  }

  std::string binary_;
  std::string output_dir_;
  uint64_t chunk_size_ = 0;
  uint32_t chunk_count_ = 0;
  uint32_t publisher_history_capacity_ = 0;
  uint32_t in_flight_margin_ = 0;
  ChildProcess child_;
  std::string ready_path_;
  std::string result_path_;
  bool started_ = false;
  bool running_ = false;
  bool clean_shutdown_ = false;
};

bool WaitForChildren(std::vector<ChildProcess>* children, int timeout_s,
                     std::string* error,
                     BenchmarkRouDiProcess* monitored_roudi = nullptr) {
  if (children == nullptr) {
    if (error != nullptr) {
      *error = "null children list";
    }
    return false;
  }
  const uint64_t deadline = MonotonicRawNowNs() +
                            static_cast<uint64_t>(std::max(1, timeout_s)) *
                                kOneSecondNs;
  std::vector<bool> done(children->size(), false);
  size_t done_count = 0;
  while (done_count < children->size()) {
    if (monitored_roudi != nullptr) {
      std::string roudi_error;
      if (!monitored_roudi->CheckRunning(&roudi_error)) {
        std::string cleanup_error;
        (void)KillAndReapChildren(children, 1000, &cleanup_error);
        if (error != nullptr) {
          *error = cleanup_error.empty()
                       ? roudi_error
                       : roudi_error + "; " + cleanup_error;
        }
        return false;
      }
    }
    bool made_progress = false;
    for (size_t idx = 0; idx < children->size(); ++idx) {
      if (done[idx]) {
        continue;
      }
      int status = 0;
      const pid_t pid = waitpid((*children)[idx].pid, &status, WNOHANG);
      if (pid == 0) {
        continue;
      }
      if (pid < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (error != nullptr) {
          *error = "waitpid failed for role=" + (*children)[idx].role +
                   " pid=" + std::to_string((*children)[idx].pid) +
                   " errno=" + std::to_string(errno);
        }
        return false;
      }
      made_progress = true;
      done[idx] = true;
      ++done_count;
      RecordChildExitStatus(status, &(*children)[idx]);
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::string cleanup_error;
        (void)KillAndReapChildren(children, 1000, &cleanup_error);
        if (error != nullptr) {
          *error = "worker failed: " + DescribeChildFailure((*children)[idx]);
          if (!cleanup_error.empty()) {
            *error += "; " + cleanup_error;
          }
        }
        return false;
      }
    }
    if (!made_progress) {
      if (MonotonicRawNowNs() >= deadline) {
        std::string cleanup_error;
        (void)KillAndReapChildren(children, 1000, &cleanup_error);
        if (error != nullptr) {
          std::ostringstream timeout;
          timeout << "worker process timeout";
          for (size_t i = 0; i < children->size(); ++i) {
            if (!done[i]) {
              timeout << " | " << DescribeChildFailure((*children)[i]);
            }
          }
          if (!cleanup_error.empty()) {
            timeout << " | " << cleanup_error;
          }
          *error = timeout.str();
        }
        return false;
      }
      SleepNs(10000000ULL);
    }
  }
  return true;
}

bool WaitForPaths(const std::vector<std::string>& paths,
                  const std::vector<ChildProcess>& children, int timeout_s,
                  const std::string& phase, std::string* error,
                  BenchmarkRouDiProcess* monitored_roudi = nullptr) {
  const uint64_t deadline =
      MonotonicRawNowNs() +
      static_cast<uint64_t>(std::max(1, timeout_s)) * kOneSecondNs;
  while (MonotonicRawNowNs() < deadline) {
    if (monitored_roudi != nullptr) {
      std::string roudi_error;
      if (!monitored_roudi->CheckRunning(&roudi_error)) {
        if (error != nullptr) {
          *error = roudi_error;
        }
        return false;
      }
    }
    bool all_exist = true;
    for (const auto& path : paths) {
      all_exist = all_exist && FileExists(path);
    }
    if (all_exist) {
      return true;
    }
    SleepNs(10 * kOneMillisecondNs);
  }
  if (error != nullptr) {
    std::ostringstream diagnostic;
    diagnostic << phase << " timeout; missing=";
    bool first = true;
    for (const auto& path : paths) {
      if (!FileExists(path)) {
        diagnostic << (first ? "" : ",") << path;
        first = false;
      }
    }
    for (const auto& child : children) {
      diagnostic << " | " << DescribeChildFailure(child);
    }
    *error = diagnostic.str();
  }
  return false;
}

std::string CreateCaseTempDir(const std::string& parent_dir, uint64_t case_id) {
  std::string tmpl = JoinPath(parent_dir, ".cyber_rt_perf_case_" +
                                          std::to_string(case_id) + "_XXXXXX");
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

enum class ComparisonMessageType {
  kBoth,
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
  std::string benchmark_roudi_binary;
  int process_case_timeout_s = 180;
  int readiness_timeout_s = 30;
  bool use_real_inter_process = true;
  bool probe_only = false;
  bool iceoryx_restart_regression = false;
  bool run_shm_zero_copy_probe = true;
  bool run_comparison_bandwidth_search = true;
  ComparisonMessageType comparison_message_type =
      ComparisonMessageType::kBoth;

  int startup_wait_ms = 2000;
  int cooldown_wait_ms = 500;
  double max_loss_rate = 0.01;
  double min_achieved_rate_ratio = 0.95;

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
  std::vector<int> publisher_scaling_counts;

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
  int iceoryx_publisher_history_capacity = 1;
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

bool NeedsBenchmarkRouDi(const BenchmarkOptions& options) {
  if (!options.use_real_inter_process) {
    return false;
  }
  if (options.probe_only) {
    return false;
  }
  if (options.iceoryx_restart_regression) {
    return true;
  }
  return options.comparison_message_type != ComparisonMessageType::kProtobuf;
}

uint32_t MaxConcurrentPublishers(const BenchmarkOptions& options) {
  (void)options;
  return 1;
}

uint32_t MaxConcurrentSubscribers(const BenchmarkOptions& options) {
  uint32_t subscribers = 1;
  if (options.run_fanout_scaling_comparison) {
    for (const int count : options.fanout_subscribers) {
      subscribers =
          std::max(subscribers, static_cast<uint32_t>(std::max(1, count)));
    }
  }
  return subscribers;
}

uint32_t BenchmarkRouDiInFlightMargin(const BenchmarkOptions& options) {
  return std::max<uint32_t>(
      8, MaxConcurrentPublishers(options) + MaxConcurrentSubscribers(options) +
             2);
}

uint32_t BenchmarkRouDiChunkCount(const BenchmarkOptions& options) {
  const uint64_t history_chunks =
      static_cast<uint64_t>(
          std::max(1, options.iceoryx_publisher_history_capacity)) *
      MaxConcurrentPublishers(options);
  const uint64_t count =
      history_chunks + BenchmarkRouDiInFlightMargin(options);
  return static_cast<uint32_t>(
      std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max()));
}

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
  double target_send_rate_hz = 0.0;
  double target_receive_rate_hz = 0.0;
  double measured_send_rate_hz = 0.0;
  double measured_receive_rate_hz = 0.0;
  double achieved_send_ratio = 0.0;
  double achieved_receive_ratio = 0.0;
  double min_achieved_rate_ratio = 0.95;
  uint64_t measured_send_duration_ns = 0;
  uint64_t measured_receive_duration_ns = 0;
  uint64_t received_messages = 0;
  uint64_t received_bytes = 0;
  uint64_t final_drained_received_messages = 0;
  uint64_t final_drained_received_bytes = 0;
  uint64_t sent_messages = 0;
  uint64_t send_failures = 0;
  uint64_t final_sent_messages = 0;
  uint64_t final_send_failures = 0;
  uint64_t loan_publish_successes = 0;
  uint64_t fallback_transmit_attempts = 0;
  uint64_t fallback_transmit_successes = 0;
  uint64_t zero_copy_borrowed_messages = 0;
  uint64_t zero_copy_copy_count = 0;
};

struct ReliabilityStats {
  double loss_rate = 0.0;
  uint64_t total_loss = 0;
  uint64_t max_consecutive_loss = 0;
  uint64_t gaps_observed = 0;
  uint64_t duplicates = 0;
  uint64_t reordered = 0;
  uint64_t duplicate_or_reordered = 0;
};

struct PublisherReliabilityStats {
  uint64_t sent_messages = 0;
  uint64_t measured_received_messages = 0;
  uint64_t received_messages = 0;
  uint64_t gaps_observed = 0;
  uint64_t duplicates = 0;
  uint64_t reordered = 0;
  uint64_t total_loss = 0;
  uint64_t max_consecutive_loss = 0;
  uint64_t last_sequence = 0;
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
  bool required_for_release = true;
  std::string error_message;
  std::string notes;

  LatencyStats latency;
  ThroughputStats throughput;
  ReliabilityStats reliability;
  std::vector<PublisherReliabilityStats> publisher_reliability;
  ResourceUsageStats resource;

  bool shm_profile_recorded = false;
  bool shm_loan_supported = false;

  uint64_t wall_time_ns = 0;
  int message_pool_depth = 0;
  std::vector<std::string> commands;
  std::vector<std::string> process_exits;
  bool endpoints_ready = false;
  bool warmup_confirmed = false;
  bool measured_delivery_confirmed = false;
  bool shutdown_confirmed = false;
  std::vector<FanoutSubscriberValidation> fanout_subscribers;
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

struct RuntimeCounters {
  std::atomic<uint64_t> received_messages{0};
  std::atomic<uint64_t> received_bytes{0};
  std::atomic<bool> accepting_messages{true};
  std::vector<std::vector<PublisherSequenceTracker>> sequence_trackers;
  std::vector<std::vector<bool>> warmup_received;
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

uint64_t ComputeSequenceCapacity(const BenchmarkOptions& options,
                                 const BenchmarkCaseConfig& config) {
  if (config.frequency_hz > 0) {
    return static_cast<uint64_t>(config.frequency_hz) *
               static_cast<uint64_t>(config.duration_s) +
           1024ULL;
  }
  return std::max<uint64_t>(
      64ULL * 1024ULL * 1024ULL,
      static_cast<uint64_t>(options.latency_sample_cap) * 16ULL);
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
    } else if (key == "--benchmark_roudi_bin") {
      options->benchmark_roudi_binary = value;
    } else if (key == "--process_case_timeout_s") {
      options->process_case_timeout_s =
          std::max(10, ParseIntOr(value, options->process_case_timeout_s));
    } else if (key == "--readiness_timeout_s") {
      options->readiness_timeout_s =
          std::max(1, ParseIntOr(value, options->readiness_timeout_s));
    } else if (key == "--use_real_inter_process") {
      options->use_real_inter_process =
          ParseBoolOr(value, options->use_real_inter_process);
    } else if (key == "--probe_only") {
      options->probe_only = ParseBoolOr(value, options->probe_only);
    } else if (key == "--iceoryx_restart_regression") {
      options->iceoryx_restart_regression =
          ParseBoolOr(value, options->iceoryx_restart_regression);
    } else if (key == "--run_shm_zero_copy_probe") {
      options->run_shm_zero_copy_probe =
          ParseBoolOr(value, options->run_shm_zero_copy_probe);
    } else if (key == "--run_comparison_bandwidth_search") {
      options->run_comparison_bandwidth_search =
          ParseBoolOr(value, options->run_comparison_bandwidth_search);
    } else if (key == "--comparison_message_type") {
      if (value == "both") {
        options->comparison_message_type = ComparisonMessageType::kBoth;
      } else if (value == "protobuf") {
        options->comparison_message_type = ComparisonMessageType::kProtobuf;
      } else if (value == "pod") {
        options->comparison_message_type = ComparisonMessageType::kPod;
      } else {
        if (error != nullptr) {
          *error =
              "--comparison_message_type must be both, protobuf, or pod";
        }
        return false;
      }
    } else if (key == "--startup_wait_ms") {
      options->startup_wait_ms = std::max(0, ParseIntOr(value, options->startup_wait_ms));
    } else if (key == "--cooldown_wait_ms") {
      options->cooldown_wait_ms =
          std::max(0, ParseIntOr(value, options->cooldown_wait_ms));
    } else if (key == "--max_loss_rate") {
      options->max_loss_rate =
          std::max(0.0, std::min(1.0, ParseDoubleOr(value, options->max_loss_rate)));
    } else if (key == "--min_achieved_rate_ratio") {
      options->min_achieved_rate_ratio = std::max(
          0.0, std::min(1.0, ParseDoubleOr(
                                  value, options->min_achieved_rate_ratio)));
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
    } else if (key == "--publisher_scaling_counts") {
      options->publisher_scaling_counts = ParsePositiveIntList(value);
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
    } else if (key == "--iceoryx_publisher_history_capacity") {
      options->iceoryx_publisher_history_capacity = std::max(
          1, ParseIntOr(value,
                        options->iceoryx_publisher_history_capacity));
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
      << "  --benchmark_roudi_bin=<path>\n"
      << "  --process_case_timeout_s=<seconds>\n"
      << "  --readiness_timeout_s=<seconds>\n"
      << "  --use_real_inter_process=true|false\n"
      << "  --probe_only=true|false\n"
      << "  --iceoryx_restart_regression=true|false\n"
      << "  --run_shm_zero_copy_probe=true|false\n"
      << "  --run_comparison_bandwidth_search=true|false\n"
      << "  --comparison_message_type=both|protobuf|pod\n"
      << "  --max_loss_rate=<0.0..1.0>\n"
      << "  --min_achieved_rate_ratio=<0.0..1.0>\n"
      << "  --publisher_scaling_counts=2,8,32\n"
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
  BenchmarkSuiteRunner(BenchmarkOptions options,
                       BenchmarkRouDiProcess* benchmark_roudi)
      : options_(std::move(options)), benchmark_roudi_(benchmark_roudi) {}

  bool Run() {
    results_.clear();
    case_counter_ = 0;
    if (benchmark_roudi_ != nullptr) {
      std::string roudi_error;
      if (!benchmark_roudi_->CheckRunning(&roudi_error)) {
        std::cerr << roudi_error << std::endl;
        return false;
      }
    }
    if (options_.iceoryx_restart_regression) {
      RunIceoryxRestartRegression();
      const bool owner_stopped =
          benchmark_roudi_ == nullptr || benchmark_roudi_->Stop();
      const bool exported = ExportResults();
      return exported && owner_stopped &&
             std::all_of(results_.begin(), results_.end(),
                         [](const BenchmarkCaseResult& result) {
                           return result.success;
                         });
    }
    if (options_.run_shm_zero_copy_probe && !RunShmZeroCopyProbe()) {
      // keep running; probe failure should be visible in report but not stop suite
    }
    if (options_.probe_only) {
      const bool owner_stopped =
          benchmark_roudi_ == nullptr || benchmark_roudi_->Stop();
      return ExportResults() && owner_stopped &&
             std::all_of(results_.begin(), results_.end(),
                         [](const BenchmarkCaseResult& result) {
                           return !result.required_for_release || result.success;
                         });
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
    const bool owner_stopped =
        benchmark_roudi_ == nullptr || benchmark_roudi_->Stop();
    const bool exported = ExportResults();
    const bool all_passed =
        std::all_of(results_.begin(), results_.end(),
                    [](const BenchmarkCaseResult& result) {
                      return !result.required_for_release || result.success;
                    });
    return exported && owner_stopped && all_passed;
  }

  const std::vector<BenchmarkCaseResult>& results() const { return results_; }

 private:
  void RunIceoryxRestartRegression() {
    BenchmarkCaseConfig config;
    config.scenario = ScenarioKind::kZeroCopyVsProtobuf;
    config.coverage = CoverageMode::kInterProcess;
    config.message_type = MessageType::kPod;
    config.topology = TopologyMode::kOnePubOneSub;
    config.publishers = 1;
    config.subscribers = 1;
    config.frequency_hz = 10;
    config.payload_bytes = static_cast<int>(kOneMegabyte);
    config.duration_s = 1;
    results_.push_back(RunSingleCase(config));
    results_.push_back(RunSingleCase(config));
  }

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
    bool loan_published = false;
    uint64_t fallback_transmit_attempts = 0;
    uint64_t fallback_transmit_successes = 0;
    if (result.shm_loan_supported) {
      transport::LoanedMessage<transport::PodMessage> loaned;
      const std::size_t required = transport::PodChunkTotalSize(payload.size());
      if (transmitter->Loan(required, &loaned)) {
        std::size_t written = 0;
        if (transport::BuildPodChunk(header, payload.data(), payload.size(),
                                     loaned.data(), loaned.capacity(), &written) &&
            loaned.set_size(written)) {
          published = transmitter->Publish(std::move(loaned));
          loan_published = published;
        }
      }
    }
    if (!published) {
      ++fallback_transmit_attempts;
      auto msg = std::make_shared<transport::PodMessage>(header, payload.data(),
                                                         payload.size());
      published = transmitter->Transmit(msg);
      fallback_transmit_successes = published ? 1 : 0;
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
    result.throughput.loan_publish_successes = loan_published ? 1 : 0;
    result.throughput.fallback_transmit_attempts =
        fallback_transmit_attempts;
    result.throughput.fallback_transmit_successes =
        fallback_transmit_successes;
    result.success = loan_published &&
                     received.load(std::memory_order_acquire) &&
                     result.shm_profile_recorded &&
                     fallback_transmit_attempts == 0;
    if (!result.success) {
      result.error_message =
          "SHM probe failed (publish/receive/profile criteria not all met)";
    }
    result.notes =
        "SHM zero-copy probe requires loan/publish delivery with no fallback.";

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
        "perf/shm_zero_copy_probe/" + std::to_string(getpid()) + "_" +
        std::to_string(MonotonicRawNowNs()) + "_" +
        std::to_string(case_suffix);
    const std::string case_dir =
        CreateCaseTempDir(DirName(options_.output_path), case_suffix);
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
      sub_child.role = "sub_probe";
      sub_child.result_path = sub_result;
      sub_child.stdout_path = JoinPath(case_dir, "sub_probe.stdout");
      sub_child.stderr_path = JoinPath(case_dir, "sub_probe.stderr");
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
      children.push_back(sub_child);
      result.commands.push_back(sub_child.command);
    }
    {
      ChildProcess pub_child;
      pub_child.role = "pub_probe";
      pub_child.result_path = pub_result;
      pub_child.stdout_path = JoinPath(case_dir, "pub_probe.stdout");
      pub_child.stderr_path = JoinPath(case_dir, "pub_probe.stderr");
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
        (void)KillAndReapChildren(&children, 1000);
        RemoveFileIfExists(sub_result);
        RemoveFileIfExists(pub_result);
        (void)rmdir(case_dir.c_str());
        results_.push_back(std::move(result));
        return false;
      }
      children.push_back(pub_child);
      result.commands.push_back(pub_child.command);
    }

    std::string wait_error;
    if (!WaitForChildren(&children,
                         std::max(options_.process_case_timeout_s, config.duration_s + 20),
                         &wait_error)) {
      result.success = false;
      result.error_message = "shm probe workers failed: " + wait_error;
      for (const auto& child : children) {
        std::ostringstream evidence;
        evidence << child.role << ":pid=" << child.pid
                 << ",exit=" << child.exit_code
                 << ",signal=" << child.term_signal;
        result.process_exits.push_back(evidence.str());
      }
      RemoveFileIfExists(sub_result);
      RemoveFileIfExists(pub_result);
      for (const auto& child : children) {
        RemoveFileIfExists(child.stdout_path);
        RemoveFileIfExists(child.stderr_path);
      }
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
    const uint64_t sent_messages =
        ParseUInt64Or(pub_kv["sent_messages"], 0);
    const uint64_t loan_publish_successes =
        ParseUInt64Or(pub_kv["loan_publish_successes"], 0);
    const uint64_t fallback_transmit_attempts =
        ParseUInt64Or(pub_kv["fallback_transmit_attempts"], 0);
    const uint64_t fallback_transmit_successes =
        ParseUInt64Or(pub_kv["fallback_transmit_successes"], 0);
    const uint64_t received_messages =
        ParseUInt64Or(sub_kv["probe_received_messages"], 0);
    const bool pub_status_ok = pub_kv["status"] == "ok";
    const bool sub_status_ok = sub_kv["status"] == "ok";
    const bool shutdown_confirmed =
        ParseIntOr(pub_kv["shutdown_complete"], 0) == 1 &&
        ParseIntOr(sub_kv["shutdown_complete"], 0) == 1;
    const bool planned_success =
        ParseIntOr(pub_kv["planned_exit_code"], -1) == 0 &&
        ParseIntOr(sub_kv["planned_exit_code"], -1) == 0;

    result.shm_loan_supported = loan_supported;
    result.shm_profile_recorded = borrowed_messages > 0 && copy_count == 0;
    const bool zero_copy_verified = borrowed_messages > 0 && copy_count == 0;
    const bool publisher_zero_copy_verified =
        sent_messages > 0 && loan_publish_successes == sent_messages &&
        fallback_transmit_attempts == 0 &&
        fallback_transmit_successes == 0;
    result.throughput.loan_publish_successes = loan_publish_successes;
    result.throughput.fallback_transmit_attempts =
        fallback_transmit_attempts;
    result.throughput.fallback_transmit_successes =
        fallback_transmit_successes;
    result.endpoints_ready = pub_status_ok && sub_status_ok;
    result.warmup_confirmed = published && received;
    result.measured_delivery_confirmed =
        sent_messages > 0 && received_messages > 0;
    result.shutdown_confirmed = shutdown_confirmed;
    result.success =
        result.endpoints_ready && result.warmup_confirmed &&
        result.measured_delivery_confirmed && result.shutdown_confirmed &&
        planned_success && loan_supported && zero_copy_verified &&
        publisher_zero_copy_verified;
    if (!result.success) {
      std::ostringstream error;
      error << "SHM zero-copy probe acceptance failed: pub_status_ok="
            << pub_status_ok << ", sub_status_ok=" << sub_status_ok
            << ", published=" << published << ", received=" << received
            << ", shutdown_confirmed=" << shutdown_confirmed
            << ", planned_success=" << planned_success
            << ", loan_supported=" << loan_supported
            << ", loan_publish_successes=" << loan_publish_successes
            << ", fallback_transmit_attempts="
            << fallback_transmit_attempts
            << ", fallback_transmit_successes="
            << fallback_transmit_successes
            << ", borrowed_messages=" << borrowed_messages
            << ", copy_count=" << copy_count
            << ", sent_messages=" << sent_messages
            << ", received_messages=" << received_messages;
      result.error_message = error.str();
    }
    std::ostringstream notes;
    notes << "real_multi_process=true"
          << " | borrowed_messages=" << borrowed_messages
          << " | copy_count=" << copy_count
          << " | loan_supported=" << (loan_supported ? "true" : "false")
          << " | loan_publish_successes=" << loan_publish_successes
          << " | fallback_transmit_attempts="
          << fallback_transmit_attempts
          << " | fallback_transmit_successes="
          << fallback_transmit_successes
          << " | zero_copy_verified=" << (zero_copy_verified ? "true" : "false")
          << " | pub_ptr=" << pub_ptr
          << " | sub_ptr=" << sub_ptr
          << " | pub_ptr_from_header=" << pub_ptr_from_header;
    result.notes = notes.str();
    for (const auto& child : children) {
      std::ostringstream evidence;
      evidence << child.role << ":pid=" << child.pid
               << ",exit=" << child.exit_code
               << ",signal=" << child.term_signal;
      result.process_exits.push_back(evidence.str());
    }

    RemoveFileIfExists(sub_result);
    RemoveFileIfExists(pub_result);
    RemoveFileIfExists(JoinPath(case_dir, "sub_probe.lat"));
    for (const auto& child : children) {
      RemoveFileIfExists(child.stdout_path);
      RemoveFileIfExists(child.stderr_path);
    }
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

    if (options_.comparison_message_type != ComparisonMessageType::kBoth) {
      BenchmarkCaseConfig selected_case = base;
      selected_case.message_type =
          options_.comparison_message_type == ComparisonMessageType::kProtobuf
              ? MessageType::kProtobuf
              : MessageType::kPod;
      results_.push_back(RunSingleCase(selected_case));
      return;
    }

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
        options_.run_comparison_bandwidth_search &&
        best_bandwidth_for(MessageType::kProtobuf, &protobuf_best);
    const bool pod_best_found =
        options_.run_comparison_bandwidth_search &&
        best_bandwidth_for(MessageType::kPod, &pod_best);
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
    if (!options_.run_comparison_bandwidth_search) {
      notes << " | max_bandwidth_search=disabled";
    } else if (!protobuf_best_found || !pod_best_found) {
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
        result.required_for_release = false;
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
      summary.success = highest_stable_hz > 0;
      if (!summary.success) {
        summary.error_message =
            "no frequency point met delivery and reliability criteria";
      }
      summary.notes = "frequency_limit_hz=" + std::to_string(highest_stable_hz);
      results_.push_back(std::move(summary));
    }
  }

  void RunBandwidthSweep() {
    for (CoverageMode coverage : AllCoverages()) {
      uint64_t highest_stable_payload_bytes = 0;
      if (coverage == CoverageMode::kInterHost) {
        BenchmarkCaseConfig anchor;
        anchor.scenario = ScenarioKind::kBandwidthSweep;
        anchor.coverage = coverage;
        anchor.topology = TopologyMode::kOnePubOneSub;
        anchor.publishers = 1;
        anchor.subscribers = 1;
        anchor.frequency_hz = options_.bandwidth_frequency_hz;
        anchor.payload_bytes = 64 * 1024;
        anchor.duration_s = options_.bandwidth_case_duration_s;
        BenchmarkCaseResult result = RunSingleCase(anchor);
        result.required_for_release = false;
        if (result.success && result.reliability.total_loss == 0 &&
            result.throughput.send_failures == 0) {
          highest_stable_payload_bytes = anchor.payload_bytes;
        }
        results_.push_back(std::move(result));
      }
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
        result.required_for_release = false;
        if (result.success && result.reliability.total_loss == 0 &&
            result.throughput.send_failures == 0) {
          highest_stable_payload_bytes =
              std::max<uint64_t>(highest_stable_payload_bytes,
                                 config.payload_bytes);
        }
        results_.push_back(std::move(result));
      }

      BenchmarkCaseResult summary;
      summary.config.scenario = ScenarioKind::kBandwidthSweep;
      summary.config.coverage = coverage;
      summary.success = highest_stable_payload_bytes > 0;
      if (!summary.success) {
        summary.error_message =
            "no bandwidth point met delivery and reliability criteria";
      }
      const double stable_bandwidth_mb_s =
          static_cast<double>(highest_stable_payload_bytes) *
          options_.bandwidth_frequency_hz / kOneMegabyte;
      std::ostringstream notes;
      notes << "bandwidth_limit_payload_bytes="
            << highest_stable_payload_bytes
            << " | bandwidth_limit_mb_s=" << stable_bandwidth_mb_s;
      summary.notes = notes.str();
      results_.push_back(std::move(summary));
    }
  }

  void RunSubscriberScaling() {
    for (CoverageMode coverage : AllCoverages()) {
      int highest_stable_subscribers = 0;
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
        BenchmarkCaseResult result = RunSingleCase(config);
        result.required_for_release = false;
        if (result.success) {
          highest_stable_subscribers =
              std::max(highest_stable_subscribers, subscribers);
        }
        results_.push_back(std::move(result));
      }
      BenchmarkCaseResult summary;
      summary.config.scenario = ScenarioKind::kSubscriberScaling;
      summary.config.coverage = coverage;
      summary.config.topology = TopologyMode::kOnePubMSub;
      summary.success = highest_stable_subscribers > 0;
      if (!summary.success) {
        summary.error_message = "no subscriber fanout point passed";
      }
      summary.notes = "subscriber_limit=" +
                      std::to_string(highest_stable_subscribers);
      results_.push_back(std::move(summary));
    }
  }

  void RunPublisherScaling() {
    for (CoverageMode coverage : AllCoverages()) {
      int highest_stable_publishers = 0;
      std::vector<int> publisher_counts = options_.publisher_scaling_counts;
      if (publisher_counts.empty()) {
        publisher_counts.reserve(static_cast<size_t>(options_.max_publishers));
        for (int publishers = 1; publishers <= options_.max_publishers;
             ++publishers) {
          publisher_counts.push_back(publishers);
        }
      }
      for (int publishers : publisher_counts) {
        BenchmarkCaseConfig config;
        config.scenario = ScenarioKind::kPublisherScaling;
        config.coverage = coverage;
        config.topology = TopologyMode::kNPubOneSub;
        config.publishers = publishers;
        config.subscribers = 1;
        config.frequency_hz = options_.scaling_frequency_hz;
        config.payload_bytes = options_.scaling_payload_bytes;
        config.duration_s = options_.scaling_case_duration_s;
        BenchmarkCaseResult result = RunSingleCase(config);
        result.required_for_release = false;
        if (result.success) {
          highest_stable_publishers =
              std::max(highest_stable_publishers, publishers);
        }
        results_.push_back(std::move(result));
      }
      BenchmarkCaseResult summary;
      summary.config.scenario = ScenarioKind::kPublisherScaling;
      summary.config.coverage = coverage;
      summary.config.topology = TopologyMode::kNPubOneSub;
      summary.success = highest_stable_publishers > 0;
      if (!summary.success) {
        summary.error_message = "no publisher fanin point passed";
      }
      summary.notes =
          "publisher_limit=" + std::to_string(highest_stable_publishers);
      results_.push_back(std::move(summary));
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
    if (config.publishers > 1 && config.subscribers > 1) {
      BenchmarkCaseResult result;
      result.config = config;
      result.success = false;
      result.error_message =
          "N-publisher/N-subscriber topology is outside benchmark scope";
      return result;
    }
    if (options_.use_real_inter_process &&
        (config.coverage == CoverageMode::kInterProcess ||
         config.coverage == CoverageMode::kInterHost)) {
      const bool uses_iceoryx =
          config.message_type == MessageType::kPod &&
          config.coverage == CoverageMode::kInterProcess;
      if (uses_iceoryx) {
        BenchmarkCaseResult result;
        result.config = config;
        std::string roudi_error;
        if (benchmark_roudi_ == nullptr ||
            !benchmark_roudi_->CheckRunning(&roudi_error)) {
          result.success = false;
          result.error_message =
              roudi_error.empty() ? "benchmark RouDi is not available"
                                  : roudi_error;
          return result;
        }
      }
      BenchmarkCaseResult result = RunSingleCaseViaWorkers(config);
      if (uses_iceoryx) {
        std::string roudi_error;
        if (!benchmark_roudi_->CheckRunning(&roudi_error)) {
          result.success = false;
          result.error_message = roudi_error;
        }
      }
      return result;
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
    const std::string case_dir =
        CreateCaseTempDir(DirName(options_.output_path), case_suffix);
    if (case_dir.empty()) {
      result.success = false;
      result.error_message = "failed to create temporary case directory";
      return result;
    }

    std::vector<std::string> sub_result_paths;
    std::vector<std::string> sub_latency_paths;
    std::vector<std::string> pub_result_paths;
    std::vector<std::string> endpoint_ready_paths;
    std::vector<std::string> warmup_ready_paths;
    sub_result_paths.reserve(static_cast<size_t>(config.subscribers));
    sub_latency_paths.reserve(static_cast<size_t>(config.subscribers));
    pub_result_paths.reserve(static_cast<size_t>(config.publishers));
    endpoint_ready_paths.reserve(
        static_cast<size_t>(config.subscribers + config.publishers));
    warmup_ready_paths.reserve(static_cast<size_t>(config.subscribers));
    const std::string measurement_start_path =
        JoinPath(case_dir, "measurement_start.kv");

    const int timeout_s =
        std::max(options_.process_case_timeout_s,
                 config.duration_s + options_.startup_wait_ms / 1000 +
                     options_.cooldown_wait_ms / 1000 + 20);
    const std::string cpu_set_text = JoinCpuSet(options_.cpu_set);
    const std::string worker_transport_mode = WorkerTransportModeForCase(config);
    const bool uses_iceoryx = worker_transport_mode == "iceoryx";
    const EnvironmentOverrides worker_environment =
        uses_iceoryx
            ? EnvironmentOverrides{{"CYBER_ICEORYX_START_ROUDI", "0"}}
            : EnvironmentOverrides{};
    const uint64_t sequence_capacity =
        ComputeSequenceCapacity(options_, config);
    const int worker_readiness_timeout_s =
        options_.readiness_timeout_s * 2 +
        std::max(1, options_.startup_wait_ms / 1000) + 5;

    std::vector<ChildProcess> children;
    auto kill_and_reap_children = [&children]() {
      (void)KillAndReapChildren(&children, 1000);
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
      for (const auto& path : endpoint_ready_paths) {
        RemoveFileIfExists(path);
      }
      for (const auto& path : warmup_ready_paths) {
        RemoveFileIfExists(path);
      }
      RemoveFileIfExists(measurement_start_path);
      for (const auto& child : children) {
        RemoveFileIfExists(child.stdout_path);
        RemoveFileIfExists(child.stderr_path);
      }
      (void)rmdir(case_dir.c_str());
    };

    for (int sub_index = 0; sub_index < config.subscribers; ++sub_index) {
      const std::string sub_result =
          JoinPath(case_dir, "sub_" + std::to_string(sub_index) + ".kv");
      const std::string sub_latency =
          JoinPath(case_dir, "sub_" + std::to_string(sub_index) + ".lat");
      const std::string endpoint_ready =
          JoinPath(case_dir,
                   "sub_" + std::to_string(sub_index) + ".endpoint_ready");
      const std::string warmup_ready =
          JoinPath(case_dir,
                   "sub_" + std::to_string(sub_index) + ".warmup_ready");
      sub_result_paths.push_back(sub_result);
      sub_latency_paths.push_back(sub_latency);
      endpoint_ready_paths.push_back(endpoint_ready);
      warmup_ready_paths.push_back(warmup_ready);

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
          "--readiness_timeout_s=" +
              std::to_string(worker_readiness_timeout_s),
          "--endpoint_ready_path=" + endpoint_ready,
          "--warmup_ready_path=" + warmup_ready,
          "--measurement_start_path=" + measurement_start_path,
          "--cpu_interference_percent=" +
              std::to_string(config.cpu_interference_percent),
          "--cooldown_wait_ms=" + std::to_string(options_.cooldown_wait_ms),
          "--latency_sample_cap=" + std::to_string(options_.latency_sample_cap),
          "--sequence_capacity=" + std::to_string(sequence_capacity),
          "--result_path=" + sub_result,
          "--latency_dump_path=" + sub_latency,
          "--cpu=" + std::to_string(cpu),
      };
      ChildProcess child;
      child.role = "sub";
      child.result_path = sub_result;
      child.stdout_path =
          JoinPath(case_dir, "sub_" + std::to_string(sub_index) + ".stdout");
      child.stderr_path =
          JoinPath(case_dir, "sub_" + std::to_string(sub_index) + ".stderr");
      std::string spawn_error;
      if (!SpawnProcess(args, &child, &spawn_error, worker_environment)) {
        result.success = false;
        result.error_message = "failed to spawn subscriber worker: " + spawn_error;
        kill_and_reap_children();
        cleanup_case_files();
        return result;
      }
      children.push_back(child);
      result.commands.push_back(child.command);
    }

    for (int pub_index = 0; pub_index < config.publishers; ++pub_index) {
      const std::string pub_result =
          JoinPath(case_dir, "pub_" + std::to_string(pub_index) + ".kv");
      const std::string endpoint_ready =
          JoinPath(case_dir,
                   "pub_" + std::to_string(pub_index) + ".endpoint_ready");
      pub_result_paths.push_back(pub_result);
      endpoint_ready_paths.push_back(endpoint_ready);
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
          "--readiness_timeout_s=" +
              std::to_string(worker_readiness_timeout_s),
          "--endpoint_ready_path=" + endpoint_ready,
          "--measurement_start_path=" + measurement_start_path,
          "--cpu_interference_percent=" +
              std::to_string(config.cpu_interference_percent),
          "--cooldown_wait_ms=" + std::to_string(options_.cooldown_wait_ms),
          "--message_pool_depth=" + std::to_string(worker_pool_depth),
          "--publisher_history_capacity=" +
              std::to_string(options_.iceoryx_publisher_history_capacity),
          "--result_path=" + pub_result,
          "--cpu=" + std::to_string(cpu),
      };
      ChildProcess child;
      child.role = "pub";
      child.result_path = pub_result;
      child.stdout_path =
          JoinPath(case_dir, "pub_" + std::to_string(pub_index) + ".stdout");
      child.stderr_path =
          JoinPath(case_dir, "pub_" + std::to_string(pub_index) + ".stderr");
      std::string spawn_error;
      if (!SpawnProcess(args, &child, &spawn_error, worker_environment)) {
        result.success = false;
        result.error_message = "failed to spawn publisher worker: " + spawn_error;
        kill_and_reap_children();
        cleanup_case_files();
        return result;
      }
      children.push_back(child);
      result.commands.push_back(child.command);
    }

    std::string readiness_error;
    if (!WaitForPaths(endpoint_ready_paths, children,
                      options_.readiness_timeout_s, "endpoint readiness",
                      &readiness_error,
                      uses_iceoryx ? benchmark_roudi_ : nullptr) ||
        !WaitForPaths(warmup_ready_paths, children,
                      options_.readiness_timeout_s, "subscriber warmup",
                      &readiness_error,
                      uses_iceoryx ? benchmark_roudi_ : nullptr)) {
      result.success = false;
      result.error_message = readiness_error;
      kill_and_reap_children();
      cleanup_case_files();
      return result;
    }
    const uint64_t measurement_start_ns =
        MonotonicRawNowNs() +
        static_cast<uint64_t>(std::max(100, options_.startup_wait_ms)) *
            kOneMillisecondNs;
    if (!WriteKvFile(
            measurement_start_path,
            {{"measurement_start_ns", std::to_string(measurement_start_ns)},
             {"endpoint_count",
              std::to_string(config.publishers + config.subscribers)},
             {"warmup_subscriber_count",
              std::to_string(config.subscribers)}})) {
      result.success = false;
      result.error_message = "failed to publish common measurement start";
      kill_and_reap_children();
      cleanup_case_files();
      return result;
    }

    std::string wait_error;
    const bool workers_ok = WaitForChildren(
        &children, timeout_s, &wait_error,
        uses_iceoryx ? benchmark_roudi_ : nullptr);
    for (const auto& child : children) {
      std::ostringstream evidence;
      evidence << child.role << ":pid=" << child.pid
               << ",exit=" << child.exit_code
               << ",signal=" << child.term_signal;
      result.process_exits.push_back(evidence.str());
    }
    if (!workers_ok) {
      result.success = false;
      result.error_message = "worker run failed: " + wait_error;
      cleanup_case_files();
      return result;
    }

    std::vector<uint64_t> sent_per_publisher(static_cast<size_t>(config.publishers),
                                             0);
    uint64_t total_sent = 0;
    uint64_t total_send_failures = 0;
    uint64_t total_loan_publish_successes = 0;
    uint64_t total_fallback_transmit_attempts = 0;
    uint64_t total_fallback_transmit_successes = 0;
    uint64_t measured_send_duration_ns = 0;
    bool any_loan_supported = false;
    bool all_loan_supported = true;
    std::string pub_transport_mode_seen;
    std::string sub_transport_mode_seen;
    uint64_t total_measured_received = 0;
    uint64_t total_measured_received_bytes = 0;
    uint64_t measured_receive_duration_ns = 0;
    uint64_t total_final_drained_received = 0;
    uint64_t total_final_drained_received_bytes = 0;
    uint64_t total_dropped_samples = 0;
    double cpu_delta_total = 0.0;
    long rss_begin_sum = 0;
    long rss_end_sum = 0;
    long voluntary_ctx_sum = 0;
    long involuntary_ctx_sum = 0;
    std::vector<uint64_t> all_latency_samples;
    bool endpoints_ready = true;
    bool shutdown_confirmed = true;
    bool warmup_confirmed = true;
    bool publishers_warmed = true;
    bool measured_delivery_confirmed = true;
    bool sequence_accounting_exact = true;
    bool fanout_subscribers_ok = true;

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
      endpoints_ready =
          endpoints_ready && ParseIntOr(kv["endpoint_ready"], 0) == 1;
      publishers_warmed =
          publishers_warmed && ParseIntOr(kv["warmup_sent"], 0) == 1;
      shutdown_confirmed =
          shutdown_confirmed && ParseIntOr(kv["shutdown_complete"], 0) == 1;
      const uint64_t sent = ParseUInt64Or(
          kv["measured_sent_messages"],
          ParseUInt64Or(kv["sent_messages"], 0));
      const uint64_t publisher_measurement_duration_ns =
          ParseUInt64Or(kv["measurement_duration_ns"], 0);
      sent_per_publisher[static_cast<size_t>(pub_index)] = sent;
      total_sent += sent;
      measured_send_duration_ns =
          std::max(measured_send_duration_ns,
                   publisher_measurement_duration_ns);
      total_send_failures += ParseUInt64Or(
          kv["measured_send_failures"],
          ParseUInt64Or(kv["send_failures"], 0));
      total_loan_publish_successes += ParseUInt64Or(
          kv["measured_loan_publish_successes"],
          ParseUInt64Or(kv["loan_publish_successes"], 0));
      total_fallback_transmit_attempts += ParseUInt64Or(
          kv["measured_fallback_transmit_attempts"],
          ParseUInt64Or(kv["fallback_transmit_attempts"], 0));
      total_fallback_transmit_successes += ParseUInt64Or(
          kv["measured_fallback_transmit_successes"],
          ParseUInt64Or(kv["fallback_transmit_successes"], 0));
      const bool publisher_loan_supported =
          ParseIntOr(kv["loan_supported"], 0) == 1;
      any_loan_supported = any_loan_supported || publisher_loan_supported;
      all_loan_supported = all_loan_supported && publisher_loan_supported;
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
    uint64_t total_gaps_observed = 0;
    uint64_t total_duplicates = 0;
    uint64_t total_reordered = 0;
    uint64_t total_borrowed = 0;
    uint64_t total_copies = 0;
    result.publisher_reliability.resize(
        static_cast<size_t>(config.publishers));

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
      const bool subscriber_endpoint_ready =
          ParseIntOr(kv["endpoint_ready"], 0) == 1;
      const bool subscriber_shutdown_confirmed =
          ParseIntOr(kv["shutdown_complete"], 0) == 1;
      const bool subscriber_result_warmup_confirmed =
          ParseIntOr(kv["warmup_confirmed"], 0) == 1;
      const uint64_t subscriber_measured_received = ParseUInt64Or(
          kv["measured_received_messages"],
          ParseUInt64Or(kv["received_messages"], 0));
      const uint64_t subscriber_measurement_duration_ns =
          ParseUInt64Or(kv["measurement_duration_ns"], 0);
      const uint64_t subscriber_final_drained_received = ParseUInt64Or(
          kv["final_received_messages"],
          ParseUInt64Or(kv["received_messages"], 0));
      endpoints_ready = endpoints_ready && subscriber_endpoint_ready;
      shutdown_confirmed =
          shutdown_confirmed && subscriber_shutdown_confirmed;
      warmup_confirmed =
          warmup_confirmed && subscriber_result_warmup_confirmed;

      total_measured_received += subscriber_measured_received;
      measured_receive_duration_ns =
          std::max(measured_receive_duration_ns,
                   subscriber_measurement_duration_ns);
      total_measured_received_bytes += ParseUInt64Or(
          kv["measured_received_bytes"],
          ParseUInt64Or(kv["received_bytes"], 0));
      total_final_drained_received += subscriber_final_drained_received;
      total_final_drained_received_bytes += ParseUInt64Or(
          kv["final_received_bytes"],
          ParseUInt64Or(kv["received_bytes"], 0));
      total_dropped_samples += ParseUInt64Or(
          kv["measured_dropped_samples"],
          ParseUInt64Or(kv["dropped_samples"], 0));
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

      bool subscriber_warmup_confirmed = true;
      bool subscriber_measured_delivery_confirmed = true;
      uint64_t subscriber_measured_received_unique = 0;
      uint64_t subscriber_final_received_unique = 0;
      uint64_t subscriber_loss = 0;
      uint64_t subscriber_max_consecutive_loss = 0;
      uint64_t subscriber_duplicates = 0;
      uint64_t subscriber_reordered = 0;
      for (int pub_index = 0; pub_index < config.publishers; ++pub_index) {
        const std::string key_prefix = "tracker_" + std::to_string(pub_index) + "_";
        const bool initialized =
            ParseIntOr(kv[key_prefix + "initialized"], 0) == 1;
        const bool tracker_warmup_confirmed =
            ParseIntOr(kv[key_prefix + "warmup_received"], 0) == 1;
        subscriber_warmup_confirmed =
            subscriber_warmup_confirmed && tracker_warmup_confirmed;
        warmup_confirmed =
            warmup_confirmed && tracker_warmup_confirmed;
        const uint64_t expected_seq =
            ParseUInt64Or(kv[key_prefix + "expected_seq"], 0);
        const uint64_t last_sequence =
            ParseUInt64Or(kv[key_prefix + "last_sequence"], 0);
        const uint64_t received_unique = ParseUInt64Or(
            kv[key_prefix + "final_received_unique"],
            ParseUInt64Or(kv[key_prefix + "received_unique"], 0));
        const uint64_t measured_received_unique = ParseUInt64Or(
            kv[key_prefix + "measured_received_unique"],
            initialized ? received_unique : 0);
        subscriber_final_received_unique += received_unique;
        subscriber_measured_received_unique += measured_received_unique;
        subscriber_measured_delivery_confirmed =
            subscriber_measured_delivery_confirmed &&
            measured_received_unique > 0;
        measured_delivery_confirmed =
            measured_delivery_confirmed && measured_received_unique > 0;
        const uint64_t gaps_observed =
            ParseUInt64Or(kv[key_prefix + "gaps_observed"], 0);
        const uint64_t tracker_max =
            ParseUInt64Or(kv[key_prefix + "max_consecutive_loss"], 0);
        const uint64_t duplicate =
            ParseUInt64Or(kv[key_prefix + "duplicates"], 0);
        const uint64_t reordered =
            ParseUInt64Or(kv[key_prefix + "reordered"], 0);
        const uint64_t out_of_window =
            ParseUInt64Or(kv[key_prefix + "out_of_window"], 0);
        const uint64_t tracker_capacity =
            ParseUInt64Or(kv[key_prefix + "sequence_capacity"], 0);
        const uint64_t sent =
            sent_per_publisher[static_cast<size_t>(pub_index)];
        sequence_accounting_exact =
            sequence_accounting_exact && out_of_window == 0 &&
            tracker_capacity >= sent && received_unique <= sent;
        uint64_t tail_loss = 0;
        if (!initialized) {
          tail_loss = sent;
        } else if (sent > expected_seq) {
          tail_loss = sent - expected_seq;
        }
        const uint64_t exact_loss =
            sent >= received_unique ? sent - received_unique : 0;
        total_loss += exact_loss;
        subscriber_loss += exact_loss;
        max_consecutive_loss =
            std::max(max_consecutive_loss, std::max(tracker_max, tail_loss));
        subscriber_max_consecutive_loss = std::max(
            subscriber_max_consecutive_loss, std::max(tracker_max, tail_loss));
        total_gaps_observed += gaps_observed;
        total_duplicates += duplicate;
        total_reordered += reordered;
        subscriber_duplicates += duplicate;
        subscriber_reordered += reordered;

        auto& publisher =
            result.publisher_reliability[static_cast<size_t>(pub_index)];
        publisher.sent_messages = sent * static_cast<uint64_t>(config.subscribers);
        publisher.measured_received_messages += measured_received_unique;
        publisher.received_messages += received_unique;
        publisher.gaps_observed += gaps_observed;
        publisher.duplicates += duplicate;
        publisher.reordered += reordered;
        publisher.total_loss += exact_loss;
        publisher.max_consecutive_loss =
            std::max(publisher.max_consecutive_loss,
                     std::max(tracker_max, tail_loss));
        publisher.last_sequence = std::max(publisher.last_sequence, last_sequence);
      }
      sequence_accounting_exact =
          sequence_accounting_exact &&
          subscriber_final_drained_received ==
              subscriber_final_received_unique;
      if (config.topology == TopologyMode::kOnePubMSub) {
        FanoutSubscriberValidation validation;
        validation.subscriber_index = sub_index;
        validation.endpoint_ready = subscriber_endpoint_ready;
        validation.warmup_confirmed = subscriber_warmup_confirmed;
        validation.measured_delivery_confirmed =
            subscriber_measured_delivery_confirmed;
        validation.shutdown_confirmed = subscriber_shutdown_confirmed;
        validation.sent_messages = total_sent;
        validation.measured_received_messages =
            subscriber_measured_received_unique;
        validation.received_messages = subscriber_final_received_unique;
        validation.final_drained_received_messages =
            subscriber_final_received_unique;
        validation.total_loss = subscriber_loss;
        validation.max_consecutive_loss = subscriber_max_consecutive_loss;
        validation.duplicates = subscriber_duplicates;
        validation.reordered = subscriber_reordered;
        validation = ValidateFanoutSubscriber(std::move(validation),
                                              options_.max_loss_rate);
        fanout_subscribers_ok = fanout_subscribers_ok && validation.success;
        result.fanout_subscribers.push_back(std::move(validation));
      }
    }
    if (config.topology == TopologyMode::kOnePubMSub) {
      fanout_subscribers_ok =
          AllFanoutSubscribersPass(result.fanout_subscribers);
    }

    result.wall_time_ns = static_cast<uint64_t>(config.duration_s) * kOneSecondNs;
    const double wall_s = SafeDiv(static_cast<double>(result.wall_time_ns), 1e9);
    result.throughput.sent_messages = total_sent;
    result.throughput.send_failures = total_send_failures;
    result.throughput.measured_send_duration_ns =
        measured_send_duration_ns;
    result.throughput.measured_receive_duration_ns =
        measured_receive_duration_ns;
    result.throughput.final_sent_messages = total_sent;
    result.throughput.final_send_failures = total_send_failures;
    result.throughput.loan_publish_successes =
        total_loan_publish_successes;
    result.throughput.fallback_transmit_attempts =
        total_fallback_transmit_attempts;
    result.throughput.fallback_transmit_successes =
        total_fallback_transmit_successes;
    result.throughput.zero_copy_borrowed_messages = total_borrowed;
    result.throughput.zero_copy_copy_count = total_copies;
    result.throughput.received_messages = total_measured_received;
    result.throughput.received_bytes = total_measured_received_bytes;
    result.throughput.final_drained_received_messages =
        total_final_drained_received;
    result.throughput.final_drained_received_bytes =
        total_final_drained_received_bytes;
    result.throughput.messages_per_s =
        SafeDiv(static_cast<double>(total_measured_received), wall_s);
    result.throughput.mb_per_s = SafeDiv(
        static_cast<double>(total_measured_received_bytes),
        wall_s * 1024.0 * 1024.0);
    const AchievedRateAcceptance achieved_rates = EvaluateAchievedRates(
        config.frequency_hz, config.publishers, config.subscribers,
        config.duration_s, total_sent, total_measured_received,
        options_.min_achieved_rate_ratio);
    result.throughput.target_send_rate_hz =
        achieved_rates.target_send_rate_hz;
    result.throughput.target_receive_rate_hz =
        achieved_rates.target_receive_rate_hz;
    result.throughput.measured_send_rate_hz =
        achieved_rates.achieved_send_rate_hz;
    result.throughput.measured_receive_rate_hz =
        achieved_rates.achieved_receive_rate_hz;
    result.throughput.achieved_send_ratio =
        achieved_rates.achieved_send_ratio;
    result.throughput.achieved_receive_ratio =
        achieved_rates.achieved_receive_ratio;
    result.throughput.min_achieved_rate_ratio =
        achieved_rates.min_achieved_rate_ratio;

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
    result.reliability.gaps_observed = total_gaps_observed;
    result.reliability.duplicates = total_duplicates;
    result.reliability.reordered = total_reordered;
    result.reliability.duplicate_or_reordered =
        total_duplicates + total_reordered;
    result.reliability.loss_rate =
        expected_total == 0
            ? 0.0
            : static_cast<double>(total_loss) / static_cast<double>(expected_total);

    result.resource.cpu_utilization_percent = SafeDiv(cpu_delta_total, wall_s) * 100.0;
    result.resource.cpu_cost_us_per_message = SafeDiv(
        cpu_delta_total * 1e6,
        static_cast<double>(
            std::max<uint64_t>(1, total_measured_received)));
    result.resource.rss_kb_begin = rss_begin_sum;
    result.resource.rss_kb_end = rss_end_sum;
    result.resource.rss_kb_peak_observed = std::max(rss_begin_sum, rss_end_sum);
    result.resource.voluntary_context_switches = voluntary_ctx_sum;
    result.resource.involuntary_context_switches = involuntary_ctx_sum;
    result.resource.context_switches = voluntary_ctx_sum + involuntary_ctx_sum;

    result.shm_loan_supported = any_loan_supported;
    result.shm_profile_recorded = total_borrowed > 0 && total_copies == 0;
    const bool pod_zero_copy_accepted =
        config.message_type != MessageType::kPod ||
        config.coverage != CoverageMode::kInterProcess ||
        (all_loan_supported && total_sent > 0 &&
         total_loan_publish_successes == total_sent &&
         total_fallback_transmit_attempts == 0 &&
         total_fallback_transmit_successes == 0 &&
         total_borrowed == total_measured_received && total_copies == 0);
    std::ostringstream notes;
    notes << "real_multi_process=true"
          << " | worker_cpu_set=" << cpu_set_text
          << " | transport_mode=" << worker_transport_mode
          << " | pub_transport_mode_seen=" << pub_transport_mode_seen
          << " | sub_transport_mode_seen=" << sub_transport_mode_seen
          << " | loan_supported="
          << (result.shm_loan_supported ? "true" : "false")
          << " | loan_publish_successes="
          << total_loan_publish_successes
          << " | fallback_transmit_attempts="
          << total_fallback_transmit_attempts
          << " | fallback_transmit_successes="
          << total_fallback_transmit_successes
          << " | achieved_send_ratio="
          << achieved_rates.achieved_send_ratio
          << " | achieved_receive_ratio="
          << achieved_rates.achieved_receive_ratio
          << " | min_achieved_rate_ratio="
          << achieved_rates.min_achieved_rate_ratio;
    if (uses_iceoryx && benchmark_roudi_ != nullptr) {
      notes << " | roudi_owner_pid=" << benchmark_roudi_->pid()
            << " | publisher_history_capacity="
            << benchmark_roudi_->publisher_history_capacity()
            << " | roudi_chunk_count="
            << benchmark_roudi_->chunk_count()
            << " | roudi_in_flight_margin="
            << benchmark_roudi_->in_flight_margin();
    }
    if (config.coverage == CoverageMode::kInterHost) {
      notes << " | inter_host_uses_separate_processes_on_single_machine";
    }
    if (config.coverage == CoverageMode::kInterProcess) {
      notes << " | zero_copy_borrowed_messages=" << total_borrowed
            << " | zero_copy_copy_count=" << total_copies;
    }
    result.notes = notes.str();
    result.endpoints_ready = endpoints_ready;
    result.warmup_confirmed = warmup_confirmed;
    result.measured_delivery_confirmed = measured_delivery_confirmed;
    result.shutdown_confirmed = shutdown_confirmed;
    result.success =
        endpoints_ready && warmup_confirmed && publishers_warmed &&
        measured_delivery_confirmed && shutdown_confirmed &&
        sequence_accounting_exact && fanout_subscribers_ok &&
        achieved_rates.accepted && pod_zero_copy_accepted &&
        total_sent > 0 && total_measured_received > 0 &&
        !all_latency_samples.empty() &&
        total_send_failures == 0 &&
        OrderingAccepted(total_duplicates, total_reordered) &&
        result.reliability.loss_rate <= options_.max_loss_rate;
    if (!result.success) {
      std::ostringstream error;
      error << "acceptance criteria failed: endpoints_ready="
            << endpoints_ready << ", warmup_confirmed=" << warmup_confirmed
            << ", publishers_warmed=" << publishers_warmed
            << ", measured_delivery_confirmed="
            << measured_delivery_confirmed
            << ", shutdown_confirmed=" << shutdown_confirmed
            << ", sequence_accounting_exact=" << sequence_accounting_exact
            << ", sent=" << total_sent
            << ", measured_received=" << total_measured_received
            << ", final_drained_received=" << total_final_drained_received
            << ", samples=" << all_latency_samples.size()
            << ", send_failures=" << total_send_failures
            << ", achieved_send_ratio="
            << achieved_rates.achieved_send_ratio
            << ", achieved_receive_ratio="
            << achieved_rates.achieved_receive_ratio
            << ", min_achieved_rate_ratio="
            << achieved_rates.min_achieved_rate_ratio
            << ", pod_zero_copy_accepted=" << pod_zero_copy_accepted
            << ", loan_publish_successes="
            << total_loan_publish_successes
            << ", fallback_transmit_attempts="
            << total_fallback_transmit_attempts
            << ", fallback_transmit_successes="
            << total_fallback_transmit_successes
            << ", borrowed_messages=" << total_borrowed
            << ", copy_count=" << total_copies
            << ", duplicates=" << total_duplicates
            << ", reordered=" << total_reordered
            << ", loss_rate=" << result.reliability.loss_rate
            << ", max_loss_rate=" << options_.max_loss_rate;
      if (!fanout_subscribers_ok) {
        error << ", fanout_subscriber_failure=true";
      }
      result.error_message = error.str();
    }

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
    MeasurementWindowMetrics measured_metrics(
        static_cast<size_t>(config.subscribers * config.publishers),
        options_.latency_sample_cap);
    const uint64_t sequence_capacity =
        ComputeSequenceCapacity(options_, config);
    runtime.sequence_trackers.resize(
        static_cast<size_t>(config.subscribers));
    for (auto& subscriber_trackers : runtime.sequence_trackers) {
      subscriber_trackers.reserve(static_cast<size_t>(config.publishers));
      for (int publisher_index = 0; publisher_index < config.publishers;
           ++publisher_index) {
        subscriber_trackers.emplace_back(sequence_capacity);
      }
    }
    runtime.warmup_received.assign(
        static_cast<size_t>(config.subscribers),
        std::vector<bool>(static_cast<size_t>(config.publishers), false));
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
            const uint64_t publisher_id = msg->lidar_timestamp();
            if (publisher_id >= static_cast<uint64_t>(config.publishers)) {
              return;
            }
            if (msg->seq() >= kBenchmarkWarmupSeqBase) {
              std::lock_guard<std::mutex> lock(
                  *runtime.sequence_locks[static_cast<size_t>(receiver_index)]);
              runtime.warmup_received[static_cast<size_t>(receiver_index)]
                                     [static_cast<size_t>(publisher_id)] = true;
              return;
            }
            SequenceObservation observation;
            {
              std::lock_guard<std::mutex> lock(
                  *runtime.sequence_locks[static_cast<size_t>(receiver_index)]);
              if (!runtime.accepting_messages.load(std::memory_order_acquire)) {
                return;
              }
              auto& tracker =
                  runtime.sequence_trackers[static_cast<size_t>(receiver_index)]
                                           [static_cast<size_t>(publisher_id)];
              observation = tracker.Observe(msg->seq());
              if (observation == SequenceObservation::kDuplicate ||
                  observation == SequenceObservation::kOutOfWindow) {
                return;
              }
              runtime.received_messages.fetch_add(1,
                                                  std::memory_order_relaxed);
              runtime.received_bytes.fetch_add(
                  static_cast<uint64_t>(msg->content().size()),
                  std::memory_order_relaxed);
            }
            const uint64_t now_ns = MonotonicRawNowNs();
            const uint64_t sent_ns = msg->timestamp();
            const uint64_t latency_ns = now_ns >= sent_ns ? now_ns - sent_ns : 0;
            const size_t endpoint_index =
                static_cast<size_t>(receiver_index * config.publishers) +
                static_cast<size_t>(publisher_id);
            (void)measured_metrics.Record(
                endpoint_index,
                static_cast<uint64_t>(msg->content().size()), latency_ns);
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
    auto snapshot_warmup_matrix = [&]() {
      std::vector<std::vector<bool>> snapshot(
          static_cast<size_t>(config.subscribers),
          std::vector<bool>(static_cast<size_t>(config.publishers), false));
      for (int receiver_index = 0; receiver_index < config.subscribers;
           ++receiver_index) {
        std::lock_guard<std::mutex> lock(
            *runtime.sequence_locks[static_cast<size_t>(receiver_index)]);
        snapshot[static_cast<size_t>(receiver_index)] =
            runtime.warmup_received[static_cast<size_t>(receiver_index)];
      }
      return snapshot;
    };
    const uint64_t warmup_deadline =
        MonotonicRawNowNs() +
        static_cast<uint64_t>(options_.readiness_timeout_s) * kOneSecondNs;
    std::vector<std::vector<bool>> warmup_matrix = snapshot_warmup_matrix();
    while (!WarmupMatrixConfirmed(
        warmup_matrix, static_cast<size_t>(config.subscribers),
        static_cast<size_t>(config.publishers))) {
      for (int publisher_index = 0; publisher_index < config.publishers;
           ++publisher_index) {
        auto warmup = std::make_shared<apollo::cyber::proto::Chatter>();
        warmup->set_content("benchmark_discovery_warmup");
        warmup->set_lidar_timestamp(static_cast<uint64_t>(publisher_index));
        warmup->set_seq(kBenchmarkWarmupSeqBase +
                        static_cast<uint64_t>(publisher_index));
        warmup->set_timestamp(MonotonicRawNowNs());
        (void)transmitters[static_cast<size_t>(publisher_index)]->Transmit(
            warmup);
      }
      SleepNs(50 * kOneMillisecondNs);
      warmup_matrix = snapshot_warmup_matrix();
      if (MonotonicRawNowNs() >= warmup_deadline) {
        result.endpoints_ready =
            receivers.size() == static_cast<size_t>(config.subscribers) &&
            transmitters.size() == static_cast<size_t>(config.publishers);
        result.warmup_confirmed = false;
        result.success = false;
        result.error_message =
            "in-process warmup timeout: " +
            MissingWarmupDiagnostics(
                warmup_matrix, static_cast<size_t>(config.subscribers),
                static_cast<size_t>(config.publishers));
        for (auto& transmitter : transmitters) {
          transmitter->Disable();
        }
        for (auto& receiver : receivers) {
          receiver->Disable();
        }
        return result;
      }
    }

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
            ++seq;
          } else {
            send_failures[static_cast<size_t>(publisher_index)].fetch_add(
                1, std::memory_order_relaxed);
          }
          if (period_ns > 0) {
            next_send_ns += period_ns;
          }
        }
      });
    }

    const ResourceSnapshot begin = CaptureResourceSnapshot();
    measured_metrics.Start();
    go.store(true, std::memory_order_release);
    const uint64_t run_start_ns = MonotonicRawNowNs();
    SleepNs(static_cast<uint64_t>(config.duration_s) * kOneSecondNs);
    stop.store(true, std::memory_order_release);
    const MeasurementWindowSnapshot measured_snapshot =
        measured_metrics.StopAndSnapshot();
    const ResourceSnapshot measurement_end = CaptureResourceSnapshot();
    const uint64_t run_end_ns = MonotonicRawNowNs();

    for (auto& sender : sender_threads) {
      if (sender.joinable()) {
        sender.join();
      }
    }

    std::vector<uint64_t> measured_sent_counts(
        static_cast<size_t>(config.publishers), 0);
    std::vector<uint64_t> measured_send_failures(
        static_cast<size_t>(config.publishers), 0);
    for (int i = 0; i < config.publishers; ++i) {
      measured_sent_counts[static_cast<size_t>(i)] =
          sent_counts[static_cast<size_t>(i)].load(std::memory_order_relaxed);
      measured_send_failures[static_cast<size_t>(i)] =
          send_failures[static_cast<size_t>(i)].load(
              std::memory_order_relaxed);
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
    result.wall_time_ns =
        run_end_ns > run_start_ns ? run_end_ns - run_start_ns : 0;

    runtime.accepting_messages.store(false, std::memory_order_release);
    for (auto& transmitter : transmitters) {
      transmitter->Disable();
    }
    for (auto& receiver : receivers) {
      receiver->Disable();
    }

    result.latency.sample_count = measured_snapshot.latency_samples.size();
    result.latency.dropped_samples = measured_snapshot.dropped_samples;
    if (!measured_snapshot.latency_samples.empty()) {
      std::vector<uint64_t> sorted = measured_snapshot.latency_samples;
      std::sort(sorted.begin(), sorted.end());
      result.latency.min_ns = sorted.front();
      result.latency.max_ns = sorted.back();
      result.latency.p50_ns = static_cast<uint64_t>(PercentileFromSorted(sorted, 50.0));
      result.latency.p95_ns = static_cast<uint64_t>(PercentileFromSorted(sorted, 95.0));
      result.latency.p99_ns = static_cast<uint64_t>(PercentileFromSorted(sorted, 99.0));
      result.latency.p999_ns =
          static_cast<uint64_t>(PercentileFromSorted(sorted, 99.9));
    }

    uint64_t total_measured_sent = 0;
    uint64_t total_measured_send_failures = 0;
    uint64_t total_sent = 0;
    uint64_t total_send_failures = 0;
    for (int i = 0; i < config.publishers; ++i) {
      total_measured_sent +=
          measured_sent_counts[static_cast<size_t>(i)];
      total_measured_send_failures +=
          measured_send_failures[static_cast<size_t>(i)];
      total_sent += sent_counts[static_cast<size_t>(i)].load(std::memory_order_relaxed);
      total_send_failures +=
          send_failures[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    }
    const uint64_t total_final_drained_received =
        runtime.received_messages.load(std::memory_order_relaxed);
    const uint64_t total_final_drained_received_bytes =
        runtime.received_bytes.load(std::memory_order_relaxed);
    const double wall_s = SafeDiv(static_cast<double>(result.wall_time_ns), 1e9);
    result.throughput.sent_messages = total_measured_sent;
    result.throughput.send_failures = total_measured_send_failures;
    result.throughput.final_sent_messages = total_sent;
    result.throughput.final_send_failures = total_send_failures;
    result.throughput.received_messages = measured_snapshot.received_messages;
    result.throughput.received_bytes = measured_snapshot.received_bytes;
    result.throughput.final_drained_received_messages =
        total_final_drained_received;
    result.throughput.final_drained_received_bytes =
        total_final_drained_received_bytes;
    result.throughput.messages_per_s =
        SafeDiv(static_cast<double>(measured_snapshot.received_messages),
                wall_s);
    result.throughput.measured_send_duration_ns = result.wall_time_ns;
    result.throughput.measured_receive_duration_ns = result.wall_time_ns;
    result.throughput.mb_per_s =
        SafeDiv(static_cast<double>(measured_snapshot.received_bytes),
                wall_s * 1024.0 * 1024.0);
    const AchievedRateAcceptance achieved_rates = EvaluateAchievedRates(
        config.frequency_hz, config.publishers, config.subscribers,
        config.duration_s, total_measured_sent,
        measured_snapshot.received_messages,
        options_.min_achieved_rate_ratio);
    result.throughput.target_send_rate_hz =
        achieved_rates.target_send_rate_hz;
    result.throughput.target_receive_rate_hz =
        achieved_rates.target_receive_rate_hz;
    result.throughput.measured_send_rate_hz =
        achieved_rates.achieved_send_rate_hz;
    result.throughput.measured_receive_rate_hz =
        achieved_rates.achieved_receive_rate_hz;
    result.throughput.achieved_send_ratio =
        achieved_rates.achieved_send_ratio;
    result.throughput.achieved_receive_ratio =
        achieved_rates.achieved_receive_ratio;
    result.throughput.min_achieved_rate_ratio =
        achieved_rates.min_achieved_rate_ratio;

    uint64_t total_loss = 0;
    uint64_t max_consecutive_loss = 0;
    uint64_t total_gaps_observed = 0;
    uint64_t total_duplicates = 0;
    uint64_t total_reordered = 0;
    bool sequence_accounting_exact = true;
    result.publisher_reliability.resize(
        static_cast<size_t>(config.publishers));
    bool fanout_subscribers_ok = true;
    for (int receiver_index = 0; receiver_index < config.subscribers;
         ++receiver_index) {
      std::lock_guard<std::mutex> lock(
          *runtime.sequence_locks[static_cast<size_t>(receiver_index)]);
      bool subscriber_warmup_confirmed = true;
      bool subscriber_measured_delivery_confirmed = true;
      uint64_t subscriber_measured_received = 0;
      uint64_t subscriber_final_received = 0;
      uint64_t subscriber_loss = 0;
      uint64_t subscriber_max_consecutive_loss = 0;
      uint64_t subscriber_duplicates = 0;
      uint64_t subscriber_reordered = 0;
      for (int publisher_index = 0; publisher_index < config.publishers;
           ++publisher_index) {
        const auto sent_count =
            sent_counts[static_cast<size_t>(publisher_index)].load(std::memory_order_relaxed);
        const auto& tracker = runtime.sequence_trackers
            [static_cast<size_t>(receiver_index)][static_cast<size_t>(publisher_index)];
        const uint64_t publisher_loss = tracker.LossForSent(sent_count);
        sequence_accounting_exact =
            sequence_accounting_exact && tracker.out_of_window() == 0 &&
            tracker.sequence_capacity() >= sent_count;
        subscriber_warmup_confirmed =
            subscriber_warmup_confirmed &&
            runtime.warmup_received[static_cast<size_t>(receiver_index)]
                                   [static_cast<size_t>(publisher_index)];
        const size_t endpoint_index =
            static_cast<size_t>(receiver_index * config.publishers +
                                publisher_index);
        const uint64_t measured_received =
            measured_snapshot.received_per_endpoint[endpoint_index];
        subscriber_measured_delivery_confirmed =
            subscriber_measured_delivery_confirmed && measured_received > 0;
        subscriber_measured_received += measured_received;
        subscriber_final_received += tracker.received_unique();
        subscriber_loss += publisher_loss;
        subscriber_max_consecutive_loss = std::max(
            subscriber_max_consecutive_loss,
            tracker.MaxConsecutiveLossForSent(sent_count));
        subscriber_duplicates += tracker.duplicates();
        subscriber_reordered += tracker.reordered();
        total_loss += publisher_loss;
        max_consecutive_loss =
            std::max(max_consecutive_loss,
                     tracker.MaxConsecutiveLossForSent(sent_count));
        total_gaps_observed += tracker.gaps_observed();
        total_duplicates += tracker.duplicates();
        total_reordered += tracker.reordered();

        auto& publisher =
            result.publisher_reliability[static_cast<size_t>(publisher_index)];
        publisher.sent_messages =
            sent_count * static_cast<uint64_t>(config.subscribers);
        publisher.measured_received_messages += measured_received;
        publisher.received_messages += tracker.received_unique();
        publisher.gaps_observed += tracker.gaps_observed();
        publisher.duplicates += tracker.duplicates();
        publisher.reordered += tracker.reordered();
        publisher.total_loss += publisher_loss;
        publisher.max_consecutive_loss =
            std::max(publisher.max_consecutive_loss,
                     tracker.MaxConsecutiveLossForSent(sent_count));
        publisher.last_sequence =
            std::max(publisher.last_sequence, tracker.last_sequence());
      }
      if (config.topology == TopologyMode::kOnePubMSub) {
        FanoutSubscriberValidation validation;
        validation.subscriber_index = receiver_index;
        validation.endpoint_ready =
            receivers.size() == static_cast<size_t>(config.subscribers);
        validation.warmup_confirmed = subscriber_warmup_confirmed;
        validation.measured_delivery_confirmed =
            subscriber_measured_delivery_confirmed;
        validation.shutdown_confirmed = true;
        validation.sent_messages = total_sent;
        validation.measured_received_messages =
            subscriber_measured_received;
        validation.received_messages = subscriber_final_received;
        validation.final_drained_received_messages =
            subscriber_final_received;
        validation.total_loss = subscriber_loss;
        validation.max_consecutive_loss = subscriber_max_consecutive_loss;
        validation.duplicates = subscriber_duplicates;
        validation.reordered = subscriber_reordered;
        validation = ValidateFanoutSubscriber(std::move(validation),
                                              options_.max_loss_rate);
        fanout_subscribers_ok = fanout_subscribers_ok && validation.success;
        result.fanout_subscribers.push_back(std::move(validation));
      }
    }
    if (config.topology == TopologyMode::kOnePubMSub) {
      fanout_subscribers_ok =
          AllFanoutSubscribersPass(result.fanout_subscribers);
    }
    const uint64_t expected_total =
        total_sent * static_cast<uint64_t>(config.subscribers);
    result.reliability.total_loss = total_loss;
    result.reliability.max_consecutive_loss = max_consecutive_loss;
    result.reliability.gaps_observed = total_gaps_observed;
    result.reliability.duplicates = total_duplicates;
    result.reliability.reordered = total_reordered;
    result.reliability.duplicate_or_reordered =
        total_duplicates + total_reordered;
    result.reliability.loss_rate =
        expected_total == 0
            ? 0.0
            : static_cast<double>(total_loss) / static_cast<double>(expected_total);

    const double cpu_begin = begin.cpu_user_s + begin.cpu_sys_s;
    const double cpu_end =
        measurement_end.cpu_user_s + measurement_end.cpu_sys_s;
    const double cpu_delta = std::max(0.0, cpu_end - cpu_begin);
    result.resource.cpu_utilization_percent = SafeDiv(cpu_delta, wall_s) * 100.0;
    result.resource.cpu_cost_us_per_message =
        SafeDiv(cpu_delta * 1e6,
                static_cast<double>(std::max<uint64_t>(
                    1, measured_snapshot.received_messages)));
    result.resource.rss_kb_begin = begin.rss_kb;
    result.resource.rss_kb_end = measurement_end.rss_kb;
    result.resource.rss_kb_peak_observed =
        std::max(begin.rss_kb, measurement_end.rss_kb);
    result.resource.voluntary_context_switches =
        measurement_end.voluntary_ctx_switches -
        begin.voluntary_ctx_switches;
    result.resource.involuntary_context_switches =
        measurement_end.involuntary_ctx_switches -
        begin.involuntary_ctx_switches;
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

    result.endpoints_ready =
        receivers.size() == static_cast<size_t>(config.subscribers) &&
        transmitters.size() == static_cast<size_t>(config.publishers);
    result.warmup_confirmed = WarmupMatrixConfirmed(
        warmup_matrix, static_cast<size_t>(config.subscribers),
        static_cast<size_t>(config.publishers));
    result.measured_delivery_confirmed =
        measured_snapshot.DeliveryConfirmed() &&
        result.latency.sample_count > 0;
    result.shutdown_confirmed = true;
    result.success =
        result.endpoints_ready && result.warmup_confirmed &&
        result.measured_delivery_confirmed && result.shutdown_confirmed &&
        sequence_accounting_exact && fanout_subscribers_ok &&
        achieved_rates.accepted &&
        total_measured_sent > 0 && measured_snapshot.received_messages > 0 &&
        result.latency.sample_count > 0 &&
        total_measured_send_failures == 0 &&
        OrderingAccepted(total_duplicates, total_reordered) &&
        result.reliability.loss_rate <= options_.max_loss_rate;
    if (!result.success) {
      std::ostringstream error;
      error << "acceptance criteria failed: endpoints_ready="
            << result.endpoints_ready
            << ", measured_sent=" << total_measured_sent
            << ", final_sent=" << total_sent
            << ", measured_received="
            << measured_snapshot.received_messages
            << ", final_drained_received=" << total_final_drained_received
            << ", samples=" << result.latency.sample_count
            << ", measured_send_failures="
            << total_measured_send_failures
            << ", final_send_failures=" << total_send_failures
            << ", achieved_send_ratio="
            << achieved_rates.achieved_send_ratio
            << ", achieved_receive_ratio="
            << achieved_rates.achieved_receive_ratio
            << ", min_achieved_rate_ratio="
            << achieved_rates.min_achieved_rate_ratio
            << ", duplicates=" << total_duplicates
            << ", reordered=" << total_reordered
            << ", sequence_accounting_exact=" << sequence_accounting_exact
            << ", loss_rate=" << result.reliability.loss_rate
            << ", max_loss_rate=" << options_.max_loss_rate;
      if (!fanout_subscribers_ok) {
        error << ", fanout_subscriber_failure=true";
      }
      result.error_message = error.str();
    }
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
    out << "  \"iceoryx_roudi\": {\n";
    out << "    \"started\": "
        << (benchmark_roudi_ != nullptr && benchmark_roudi_->started()
                ? "true"
                : "false")
        << ",\n";
    out << "    \"pid\": "
        << (benchmark_roudi_ == nullptr ? -1 : benchmark_roudi_->pid())
        << ",\n";
    out << "    \"chunk_size\": "
        << (benchmark_roudi_ == nullptr ? 0
                                        : benchmark_roudi_->chunk_size())
        << ",\n";
    out << "    \"chunk_count\": "
        << (benchmark_roudi_ == nullptr ? 0
                                        : benchmark_roudi_->chunk_count())
        << ",\n";
    out << "    \"publisher_history_capacity\": "
        << (benchmark_roudi_ == nullptr
                ? 0
                : benchmark_roudi_->publisher_history_capacity())
        << ",\n";
    out << "    \"in_flight_margin\": "
        << (benchmark_roudi_ == nullptr
                ? 0
                : benchmark_roudi_->in_flight_margin())
        << ",\n";
    out << "    \"clean_shutdown\": "
        << (benchmark_roudi_ == nullptr || benchmark_roudi_->clean_shutdown()
                ? "true"
                : "false")
        << ",\n";
    out << "    \"exit_code\": "
        << (benchmark_roudi_ == nullptr ? 0
                                        : benchmark_roudi_->exit_code())
        << ",\n";
    out << "    \"term_signal\": "
        << (benchmark_roudi_ == nullptr ? 0
                                        : benchmark_roudi_->term_signal())
        << "\n";
    out << "  },\n";
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
      out << "      \"required_for_release\": "
          << (result.required_for_release ? "true" : "false") << ",\n";
      out << "      \"error_message\": \"" << JsonEscape(result.error_message)
          << "\",\n";
      out << "      \"notes\": \"" << JsonEscape(result.notes) << "\",\n";
      out << "      \"wall_time_ns\": " << result.wall_time_ns << ",\n";
      out << "      \"message_pool_depth\": " << result.message_pool_depth << ",\n";
      out << "      \"shm_profile_recorded\": "
          << (result.shm_profile_recorded ? "true" : "false") << ",\n";
      out << "      \"shm_loan_supported\": "
          << (result.shm_loan_supported ? "true" : "false") << ",\n";
      out << "      \"execution\": {\n";
      out << "        \"endpoints_ready\": "
          << (result.endpoints_ready ? "true" : "false") << ",\n";
      out << "        \"warmup_confirmed\": "
          << (result.warmup_confirmed ? "true" : "false") << ",\n";
      out << "        \"measured_delivery_confirmed\": "
          << (result.measured_delivery_confirmed ? "true" : "false")
          << ",\n";
      out << "        \"shutdown_confirmed\": "
          << (result.shutdown_confirmed ? "true" : "false") << ",\n";
      out << "        \"commands\": [";
      for (size_t command_index = 0; command_index < result.commands.size();
           ++command_index) {
        out << (command_index == 0 ? "" : ", ") << "\""
            << JsonEscape(result.commands[command_index]) << "\"";
      }
      out << "],\n";
      out << "        \"process_exits\": [";
      for (size_t exit_index = 0; exit_index < result.process_exits.size();
           ++exit_index) {
        out << (exit_index == 0 ? "" : ", ") << "\""
            << JsonEscape(result.process_exits[exit_index]) << "\"";
      }
      out << "]\n";
      out << "      },\n";

      out << "      \"fanout_subscribers\": [";
      for (size_t sub_index = 0;
           sub_index < result.fanout_subscribers.size(); ++sub_index) {
        const auto& subscriber = result.fanout_subscribers[sub_index];
        out << (sub_index == 0 ? "\n" : ",\n");
        out << "        {\"subscriber_index\": "
            << subscriber.subscriber_index
            << ", \"success\": "
            << (subscriber.success ? "true" : "false")
            << ", \"endpoint_ready\": "
            << (subscriber.endpoint_ready ? "true" : "false")
            << ", \"warmup_confirmed\": "
            << (subscriber.warmup_confirmed ? "true" : "false")
            << ", \"measured_delivery_confirmed\": "
            << (subscriber.measured_delivery_confirmed ? "true" : "false")
            << ", \"shutdown_confirmed\": "
            << (subscriber.shutdown_confirmed ? "true" : "false")
            << ", \"sent_messages\": " << subscriber.sent_messages
            << ", \"measured_received_messages\": "
            << subscriber.measured_received_messages
            << ", \"received_messages\": " << subscriber.received_messages
            << ", \"final_drained_received_messages\": "
            << subscriber.final_drained_received_messages
            << ", \"total_loss\": " << subscriber.total_loss
            << ", \"loss_rate\": " << subscriber.loss_rate
            << ", \"max_consecutive_loss\": "
            << subscriber.max_consecutive_loss
            << ", \"duplicates\": " << subscriber.duplicates
            << ", \"reordered\": " << subscriber.reordered
            << ", \"error_message\": \""
            << JsonEscape(subscriber.error_message) << "\"}";
      }
      if (!result.fanout_subscribers.empty()) {
        out << "\n      ";
      }
      out << "],\n";

      out << "      \"latency\": {\n";
      out << "        \"window\": \"measurement\",\n";
      out << "        \"min_ns\": " << result.latency.min_ns << ",\n";
      out << "        \"p50_ns\": " << result.latency.p50_ns << ",\n";
      out << "        \"p95_ns\": " << result.latency.p95_ns << ",\n";
      out << "        \"p99_ns\": " << result.latency.p99_ns << ",\n";
      out << "        \"p99_9_ns\": " << result.latency.p999_ns << ",\n";
      out << "        \"max_ns\": " << result.latency.max_ns << ",\n";
      out << "        \"sample_count\": " << result.latency.sample_count << ",\n";
      out << "        \"measured_sample_count\": "
          << result.latency.sample_count << ",\n";
      out << "        \"dropped_samples\": " << result.latency.dropped_samples
          << "\n";
      out << "      },\n";

      out << "      \"throughput\": {\n";
      out << "        \"window\": \"measurement\",\n";
      out << "        \"messages_per_s\": " << result.throughput.messages_per_s
          << ",\n";
      out << "        \"mb_per_s\": " << result.throughput.mb_per_s << ",\n";
      out << "        \"target_send_rate_hz\": "
          << result.throughput.target_send_rate_hz << ",\n";
      out << "        \"target_receive_rate_hz\": "
          << result.throughput.target_receive_rate_hz << ",\n";
      out << "        \"measured_send_rate_hz\": "
          << result.throughput.measured_send_rate_hz << ",\n";
      out << "        \"measured_receive_rate_hz\": "
          << result.throughput.measured_receive_rate_hz << ",\n";
      out << "        \"achieved_send_ratio\": "
          << result.throughput.achieved_send_ratio << ",\n";
      out << "        \"achieved_receive_ratio\": "
          << result.throughput.achieved_receive_ratio << ",\n";
      out << "        \"min_achieved_rate_ratio\": "
          << result.throughput.min_achieved_rate_ratio << ",\n";
      out << "        \"measured_send_duration_ns\": "
          << result.throughput.measured_send_duration_ns << ",\n";
      out << "        \"measured_receive_duration_ns\": "
          << result.throughput.measured_receive_duration_ns << ",\n";
      out << "        \"sent_messages\": " << result.throughput.sent_messages
          << ",\n";
      out << "        \"measured_sent_messages\": "
          << result.throughput.sent_messages << ",\n";
      out << "        \"send_failures\": " << result.throughput.send_failures
          << ",\n";
      out << "        \"measured_send_failures\": "
          << result.throughput.send_failures << ",\n";
      out << "        \"final_sent_messages\": "
          << result.throughput.final_sent_messages << ",\n";
      out << "        \"final_send_failures\": "
          << result.throughput.final_send_failures << ",\n";
      out << "        \"loan_publish_successes\": "
          << result.throughput.loan_publish_successes << ",\n";
      out << "        \"measured_loan_publish_successes\": "
          << result.throughput.loan_publish_successes << ",\n";
      out << "        \"fallback_transmit_attempts\": "
          << result.throughput.fallback_transmit_attempts << ",\n";
      out << "        \"measured_fallback_transmit_attempts\": "
          << result.throughput.fallback_transmit_attempts << ",\n";
      out << "        \"fallback_transmit_successes\": "
          << result.throughput.fallback_transmit_successes << ",\n";
      out << "        \"measured_fallback_transmit_successes\": "
          << result.throughput.fallback_transmit_successes << ",\n";
      out << "        \"zero_copy_borrowed_messages\": "
          << result.throughput.zero_copy_borrowed_messages << ",\n";
      out << "        \"zero_copy_copy_count\": "
          << result.throughput.zero_copy_copy_count << ",\n";
      out << "        \"received_messages\": "
          << result.throughput.received_messages << ",\n";
      out << "        \"measured_received_messages\": "
          << result.throughput.received_messages << ",\n";
      out << "        \"received_bytes\": " << result.throughput.received_bytes
          << ",\n";
      out << "        \"measured_received_bytes\": "
          << result.throughput.received_bytes << ",\n";
      out << "        \"final_drained_received_messages\": "
          << result.throughput.final_drained_received_messages << ",\n";
      out << "        \"final_drained_received_bytes\": "
          << result.throughput.final_drained_received_bytes << "\n";
      out << "      },\n";

      out << "      \"reliability\": {\n";
      out << "        \"window\": \"final_drained\",\n";
      out << "        \"loss_rate\": " << result.reliability.loss_rate << ",\n";
      out << "        \"total_loss\": " << result.reliability.total_loss << ",\n";
      out << "        \"final_drained_total_loss\": "
          << result.reliability.total_loss << ",\n";
      out << "        \"max_consecutive_loss\": "
          << result.reliability.max_consecutive_loss << ",\n";
      out << "        \"gaps_observed\": "
          << result.reliability.gaps_observed << ",\n";
      out << "        \"duplicates\": " << result.reliability.duplicates
          << ",\n";
      out << "        \"reordered\": " << result.reliability.reordered
          << ",\n";
      out << "        \"duplicate_or_reordered\": "
          << result.reliability.duplicate_or_reordered << ",\n";
      out << "        \"per_publisher\": [\n";
      for (size_t publisher_index = 0;
           publisher_index < result.publisher_reliability.size();
           ++publisher_index) {
        const auto& publisher =
            result.publisher_reliability[publisher_index];
        const double publisher_loss_rate =
            publisher.sent_messages == 0
                ? 0.0
                : static_cast<double>(publisher.total_loss) /
                      static_cast<double>(publisher.sent_messages);
        out << "          {\"publisher_id\": " << publisher_index
            << ", \"sent_messages\": " << publisher.sent_messages
            << ", \"measured_received_messages\": "
            << publisher.measured_received_messages
            << ", \"received_messages\": " << publisher.received_messages
            << ", \"final_drained_received_messages\": "
            << publisher.received_messages
            << ", \"last_sequence\": " << publisher.last_sequence
            << ", \"gaps_observed\": " << publisher.gaps_observed
            << ", \"duplicates\": " << publisher.duplicates
            << ", \"reordered\": " << publisher.reordered
            << ", \"total_loss\": " << publisher.total_loss
            << ", \"max_consecutive_loss\": "
            << publisher.max_consecutive_loss
            << ", \"loss_rate\": " << publisher_loss_rate << "}"
            << (publisher_index + 1 < result.publisher_reliability.size()
                    ? ","
                    : "")
            << "\n";
      }
      out << "        ]\n";
      out << "      },\n";

      out << "      \"resource\": {\n";
      out << "        \"cpu_cost_us_per_message\": "
          << result.resource.cpu_cost_us_per_message << ",\n";
      out << "        \"measured_cpu_cost_us_per_message\": "
          << result.resource.cpu_cost_us_per_message << ",\n";
      out << "        \"cpu_cost_message_window\": \"measurement\",\n";
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
  BenchmarkRouDiProcess* benchmark_roudi_ = nullptr;
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
  if (options.benchmark_roudi_binary.empty()) {
    options.benchmark_roudi_binary =
        apollo::cyber::examples::perf_test::JoinPath(bin_dir,
                                                     "benchmark_roudi");
  }

  std::unique_ptr<apollo::cyber::examples::perf_test::BenchmarkRouDiProcess>
      benchmark_roudi;
  if (apollo::cyber::examples::perf_test::NeedsBenchmarkRouDi(options)) {
    benchmark_roudi = std::make_unique<
        apollo::cyber::examples::perf_test::BenchmarkRouDiProcess>(
        options.benchmark_roudi_binary,
        apollo::cyber::examples::perf_test::DirName(options.output_path),
        8ULL * 1024ULL * 1024ULL,
        apollo::cyber::examples::perf_test::BenchmarkRouDiChunkCount(options),
        static_cast<uint32_t>(
            options.iceoryx_publisher_history_capacity),
        apollo::cyber::examples::perf_test::BenchmarkRouDiInFlightMargin(
            options));
    std::string roudi_error;
    if (!benchmark_roudi->Start(&roudi_error)) {
      std::cerr << "failed to start benchmark RouDi: " << roudi_error
                << std::endl;
      return 1;
    }
  }

  if (!options.cpu_set.empty()) {
    (void)apollo::cyber::examples::perf_test::PinCurrentThreadToCpu(
        options.cpu_set.front());
  }

  apollo::cyber::Init(argv[0]);
  apollo::cyber::transport::Transport::Instance();

  apollo::cyber::examples::perf_test::BenchmarkSuiteRunner runner(
      std::move(options), benchmark_roudi.get());
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

/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
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

#include "cyber/scheduler/common/pin_thread.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <sys/resource.h>

namespace apollo {
namespace cyber {
namespace scheduler {

namespace {

bool ParseCpuId(const std::string& value, int* cpu) {
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(),
                   [](unsigned char c) { return std::isdigit(c) != 0; })) {
    return false;
  }

  errno = 0;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0' ||
      parsed >= CPU_SETSIZE ||
      parsed > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
    return false;
  }

  *cpu = static_cast<int>(parsed);
  return true;
}

}  // namespace

bool ParseCpuset(const std::string& str, std::vector<int>* cpuset) {
  if (cpuset == nullptr) {
    AERROR << "cpuset output is null";
    return false;
  }
  if (str.empty()) {
    return true;
  }
  if (str.back() == ',') {
    AERROR << "invalid cpuset: " << str;
    return false;
  }

  std::vector<int> parsed_cpus;
  std::stringstream ss(str);
  std::string token;
  while (getline(ss, token, ',')) {
    token.erase(std::remove_if(token.begin(), token.end(),
                               [](unsigned char c) {
                                 return std::isspace(c) != 0;
                               }),
                token.end());
    const auto dash = token.find('-');
    if (token.empty() ||
        (dash != std::string::npos && token.find('-', dash + 1) !=
                                          std::string::npos)) {
      AERROR << "invalid cpuset: " << str;
      return false;
    }

    int begin = 0;
    int end = 0;
    if (dash == std::string::npos) {
      if (!ParseCpuId(token, &begin)) {
        AERROR << "invalid cpuset: " << str;
        return false;
      }
      end = begin;
    } else if (!ParseCpuId(token.substr(0, dash), &begin) ||
               !ParseCpuId(token.substr(dash + 1), &end) || begin > end) {
      AERROR << "invalid cpuset: " << str;
      return false;
    }

    for (int cpu = begin; cpu <= end; ++cpu) {
      parsed_cpus.push_back(cpu);
    }
  }
  cpuset->insert(cpuset->end(), parsed_cpus.begin(), parsed_cpus.end());
  return true;
}

bool SetSchedAffinity(std::thread* thread, const std::vector<int>& cpus,
                      const std::string& affinity, int cpu_id) {
  if (thread == nullptr) {
    AERROR << "thread is null";
    return false;
  }
  if (cpus.empty()) {
    return true;
  }

  cpu_set_t set;
  CPU_ZERO(&set);

  if (affinity == "range") {
    for (const auto cpu : cpus) {
      if (cpu < 0 || cpu >= CPU_SETSIZE) {
        AERROR << "invalid CPU " << cpu << " in affinity set";
        return false;
      }
      CPU_SET(cpu, &set);
    }
  } else if (affinity == "1to1") {
    if (cpu_id < 0 || static_cast<size_t>(cpu_id) >= cpus.size()) {
      AERROR << "invalid CPU index " << cpu_id << " for 1to1 affinity";
      return false;
    }
    const int cpu = cpus[cpu_id];
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
      AERROR << "invalid CPU " << cpu << " in affinity set";
      return false;
    }
    CPU_SET(cpu, &set);
  } else {
    AERROR << "unknown affinity policy: " << affinity;
    return false;
  }

  const int result =
      pthread_setaffinity_np(thread->native_handle(), sizeof(set), &set);
  if (result != 0) {
    AERROR << "failed to set " << affinity << " affinity for thread "
           << thread->get_id() << ": " << strerror(result);
    return false;
  }
  AINFO << "thread " << thread->get_id() << " set " << affinity
        << " affinity";
  return true;
}

bool SetSchedPolicy(std::thread* thread, std::string spolicy,
                    int sched_priority, pid_t tid) {
  if (thread == nullptr) {
    AERROR << "thread is null";
    return false;
  }
  if (spolicy.empty()) {
    return true;
  }

  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = sched_priority;

  if (spolicy == "SCHED_FIFO" || spolicy == "SCHED_RR") {
    const int policy = spolicy == "SCHED_FIFO" ? SCHED_FIFO : SCHED_RR;
    const int result =
        pthread_setschedparam(thread->native_handle(), policy, &sp);
    if (result != 0) {
      AERROR << "failed to set " << spolicy << " for thread "
             << thread->get_id() << ": " << strerror(result);
      return false;
    }
  } else if (spolicy == "SCHED_OTHER") {
    if (sched_priority < -20 || sched_priority > 19) {
      AERROR << "invalid SCHED_OTHER priority: " << sched_priority;
      return false;
    }
    const id_t target = tid < 0 ? 0 : tid;
    if (setpriority(PRIO_PROCESS, target, sched_priority) != 0) {
      AERROR << "failed to set SCHED_OTHER priority for tid " << target
             << ": " << strerror(errno);
      return false;
    }
  } else {
    AERROR << "unknown scheduling policy: " << spolicy;
    return false;
  }

  AINFO << "thread " << tid << " set sched_policy: " << spolicy;
  return true;
}

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo

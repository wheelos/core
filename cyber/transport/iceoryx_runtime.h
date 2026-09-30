// Copyright 2025 WheelOS. All Rights Reserved.
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

#ifndef CYBER_TRANSPORT_ICEORYX_RUNTIME_H_
#define CYBER_TRANSPORT_ICEORYX_RUNTIME_H_

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <fcntl.h>
#include <signal.h>
#include <mutex>
#include <string>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/proto/role_attributes.pb.h"
#include "iceoryx_posh/iceoryx_posh_config.hpp"
#include "iceoryx_posh/internal/roudi/roudi.hpp"
#include "iceoryx_posh/roudi/iceoryx_roudi_components.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"

namespace apollo {
namespace cyber {
namespace transport {

inline std::once_flag g_iceoryx_runtime_once;
inline iox::roudi::IceOryxRouDiComponents* g_iceoryx_roudi_components = nullptr;
inline iox::roudi::RouDi* g_iceoryx_roudi = nullptr;
inline int g_iceoryx_roudi_lock_fd = -1;
inline pid_t g_iceoryx_roudi_child_pid = -1;

inline bool IceoryxEnvFlagEnabled(const std::string& name,
                                  bool default_value) {
  const char* env = std::getenv(name.c_str());
  if (env == nullptr || *env == '\0') {
    return default_value;
  }
  const std::string value(env);
  return value != "0" && value != "false" && value != "FALSE";
}

inline uint64_t IceoryxEnvUint64(const std::string& name,
                                 uint64_t default_value) {
  const char* env = std::getenv(name.c_str());
  if (env == nullptr || *env == '\0') {
    return default_value;
  }
  const std::string value(env);
  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtoull(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    AERROR << "invalid " << name << "=" << value
           << ", using default " << default_value;
    return default_value;
  }
  return parsed;
}

inline void EnsureIceoryxRouDiDaemon() {
  if (g_iceoryx_roudi != nullptr) {
    return;
  }
  if (!IceoryxEnvFlagEnabled("CYBER_ICEORYX_START_ROUDI", true)) {
    AINFO << "CYBER_ICEORYX_START_ROUDI disabled; expecting external RouDi";
    return;
  }

  const char* lock_env = std::getenv("CYBER_ICEORYX_ROUDI_LOCK");
  const std::string lock_path =
      (lock_env == nullptr || *lock_env == '\0')
          ? "/tmp/cyber_iceoryx_roudi.lock"
          : lock_env;
  // Leave the lock file in place so all processes flock the same inode.
  constexpr int kReadFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
  g_iceoryx_roudi_lock_fd = ::open(lock_path.c_str(), kReadFlags);
  if (g_iceoryx_roudi_lock_fd < 0 && errno == ENOENT) {
    g_iceoryx_roudi_lock_fd =
        ::open(lock_path.c_str(), kReadFlags | O_CREAT | O_EXCL, 0644);
    if (g_iceoryx_roudi_lock_fd >= 0) {
      if (::fchmod(g_iceoryx_roudi_lock_fd, 0644) != 0) {
        AERROR << "failed to set iceoryx RouDi lock permissions, errno: "
               << errno;
        ::close(g_iceoryx_roudi_lock_fd);
        g_iceoryx_roudi_lock_fd = -1;
        return;
      }
    } else if (errno == EEXIST) {
      g_iceoryx_roudi_lock_fd = ::open(lock_path.c_str(), kReadFlags);
    }
  }
  if (g_iceoryx_roudi_lock_fd < 0) {
    AERROR << "failed to open iceoryx RouDi lock file " << lock_path
           << ", errno: " << errno;
    return;
  }

  if (::flock(g_iceoryx_roudi_lock_fd, LOCK_EX | LOCK_NB) != 0) {
    ADEBUG << "iceoryx RouDi daemon already running in another process";
    return;
  }

  pid_t child_pid = ::fork();
  if (child_pid < 0) {
    AERROR << "failed to fork iceoryx RouDi daemon";
    return;
  }
  if (child_pid > 0) {
    g_iceoryx_roudi_child_pid = child_pid;
    ::close(g_iceoryx_roudi_lock_fd);
    g_iceoryx_roudi_lock_fd = -1;
    ::sleep(1);
    return;
  }

  if (IceoryxEnvFlagEnabled("CYBER_ICEORYX_ROUDI_TIE_TO_PARENT", true)) {
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
      AERROR << "failed to bind embedded iceoryx RouDi to parent lifetime";
    }
    if (::getppid() == 1) {
      ::_exit(1);
    }
  }
  ::prctl(PR_SET_NAME, "cyber_iox_roudi", 0, 0, 0);
  ::setsid();
  iox::IceoryxConfig config = iox::IceoryxConfig().setDefaults();
  config.sharesAddressSpaceWithApplications = false;
  const auto large_chunk_size = IceoryxEnvUint64(
      "CYBER_ICEORYX_MEMPOOL_CHUNK_SIZE", 8u * 1024u * 1024u);
  const auto large_chunk_count = static_cast<uint32_t>(IceoryxEnvUint64(
      "CYBER_ICEORYX_MEMPOOL_CHUNK_COUNT", 16u));
  for (auto& segment : config.m_sharedMemorySegments) {
    segment.m_mempoolConfig.addMemPool({large_chunk_size, large_chunk_count});
  }
  g_iceoryx_roudi_components =
      new iox::roudi::IceOryxRouDiComponents(config);
  g_iceoryx_roudi = new iox::roudi::RouDi(
      g_iceoryx_roudi_components->rouDiMemoryManager,
      g_iceoryx_roudi_components->portManager, config);

  while (true) {
    ::pause();
  }
}

inline void EnsureIceoryxRuntime(const proto::RoleAttributes& attr) {
  std::call_once(g_iceoryx_runtime_once, [&attr]() {
    EnsureIceoryxRouDiDaemon();
    const std::string runtime_name =
        "cyber_iox_" + std::to_string(common::GlobalData::Instance()->ProcessId()) +
        "_" + std::to_string(common::Hash(attr.channel_name()));
    iox::runtime::PoshRuntime::initRuntime(
        iox::RuntimeName_t{iox::TruncateToCapacity, runtime_name.c_str()});
  });
}

inline void ShutdownIceoryxRouDiDaemon() {
  const pid_t child_pid = g_iceoryx_roudi_child_pid;
  g_iceoryx_roudi_child_pid = -1;
  if (child_pid <= 0) {
    return;
  }

  if (::kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
    AERROR << "failed to stop embedded iceoryx RouDi pid=" << child_pid;
  }
  for (int i = 0; i < 40; ++i) {
    const pid_t waited = ::waitpid(child_pid, nullptr, WNOHANG);
    if (waited == child_pid || (waited < 0 && errno == ECHILD)) {
      return;
    }
    ::usleep(50000);
  }

  if (::kill(child_pid, SIGKILL) != 0 && errno != ESRCH) {
    AERROR << "failed to force-stop embedded iceoryx RouDi pid=" << child_pid;
  }
  (void)::waitpid(child_pid, nullptr, 0);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_ICEORYX_RUNTIME_H_

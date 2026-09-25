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

#include "cyber/transport/nvsci/gpu_channel_manager.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cyber/common/util.h"

namespace apollo {
namespace cyber {
namespace transport {
namespace {

constexpr char kChildChannelEnv[] = "CYBER_GPU_WRITER_LOCK_CHILD_CHANNEL";
constexpr char kChildExpectedEnv[] = "CYBER_GPU_WRITER_LOCK_CHILD_EXPECTED";

int RunLockChild() {
  const char* channel_name = std::getenv(kChildChannelEnv);
  const char* expected_value = std::getenv(kChildExpectedEnv);
  if (channel_name == nullptr || expected_value == nullptr) {
    return 2;
  }

  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 64;
  auto* manager = GpuChannelManager::Instance();
  auto session = manager->GetOrCreateSession(channel_name, config);
  if (!session) {
    return 3;
  }

  const bool claimed = manager->ClaimWriterSession(channel_name, session);
  const bool expected = std::string(expected_value) == "1";
  if (claimed && std::string(expected_value) == "exit") {
    ::_exit(0);
  }
  if (claimed) {
    manager->ReleaseWriterSession(channel_name, session->session_id());
  }
  return claimed == expected ? 0 : 4;
}

bool RunLockChildProcess(const std::string& channel_name,
                         const std::string& expected) {
  const pid_t child = ::fork();
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    if (::setenv(kChildChannelEnv, channel_name.c_str(), 1) != 0 ||
        ::setenv(kChildExpectedEnv, expected.c_str(), 1) != 0) {
      ::_exit(126);
    }
    ::execl("/proc/self/exe", "gpu_channel_manager_test",
            static_cast<char*>(nullptr));
    ::_exit(127);
  }

  int status = 0;
  if (::waitpid(child, &status, 0) != child) {
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

struct WriterClaimGuard {
  GpuChannelManager* manager;
  const std::string& channel_name;
  uint64_t session_id;
  bool active = true;

  void Release() {
    if (active) {
      manager->ReleaseWriterSession(channel_name, session_id);
      active = false;
    }
  }

  ~WriterClaimGuard() { Release(); }
};

class GpuChannelManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* previous = std::getenv("XDG_RUNTIME_DIR");
    if (previous != nullptr) {
      previous_runtime_dir_ = previous;
      had_runtime_dir_ = true;
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      previous_home_ = home;
      had_home_ = true;
    }
    char path[] = "/tmp/cyber_gpu_lock_test_XXXXXX";
    char* created = ::mkdtemp(path);
    ASSERT_NE(created, nullptr);
    runtime_dir_ = created;
    ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", runtime_dir_.c_str(), 1), 0);
  }

  void TearDown() override {
    GpuChannelManager::Instance()->Clear();
    if (had_runtime_dir_) {
      EXPECT_EQ(::setenv("XDG_RUNTIME_DIR", previous_runtime_dir_.c_str(), 1),
                0);
    } else {
      EXPECT_EQ(::unsetenv("XDG_RUNTIME_DIR"), 0);
    }
    if (had_home_) {
      EXPECT_EQ(::setenv("HOME", previous_home_.c_str(), 1), 0);
    } else {
      EXPECT_EQ(::unsetenv("HOME"), 0);
    }
    for (const auto& path : cleanup_files_) {
      struct stat info{};
      if (::lstat(path.c_str(), &info) == 0) {
        EXPECT_EQ(::unlink(path.c_str()), 0) << path;
      }
    }
    if (!runtime_dir_.empty()) {
      const std::string lock_dir = runtime_dir_ + "/cyber_gpu_writer";
      if (::access(lock_dir.c_str(), F_OK) == 0) {
        EXPECT_EQ(::rmdir(lock_dir.c_str()), 0);
      }
      EXPECT_EQ(::rmdir(runtime_dir_.c_str()), 0);
    }
  }

  std::string UniqueChannel() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const std::string channel = "test/gpu_writer_lock_" +
                                std::to_string(::getpid()) + "_" +
                                std::to_string(now.count());
    cleanup_files_.push_back(runtime_dir_ + "/cyber_gpu_writer/" +
                             std::to_string(common::Hash(channel)) + ".lock");
    return channel;
  }

  GpuChannelSessionPtr NewSession(const std::string& channel) {
    NvSciBufPoolConfig config;
    config.slot_count = 1;
    config.slot_size = 64;
    return GpuChannelManager::Instance()->GetOrCreateSession(channel, config);
  }

  std::string runtime_dir_;
  std::vector<std::string> cleanup_files_;

 private:
  std::string previous_runtime_dir_;
  std::string previous_home_;
  bool had_runtime_dir_ = false;
  bool had_home_ = false;
};

}  // namespace

TEST_F(GpuChannelManagerTest, WriterClaimIsExclusiveAcrossProcesses) {
  auto* manager = GpuChannelManager::Instance();
  const std::string channel_name = UniqueChannel();
  auto session = NewSession(channel_name);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(manager->ClaimWriterSession(channel_name, session));
  WriterClaimGuard claim{manager, channel_name, session->session_id()};

  EXPECT_TRUE(RunLockChildProcess(channel_name, "0"));

  claim.Release();
  EXPECT_TRUE(RunLockChildProcess(channel_name, "1"));
  EXPECT_EQ(manager->GetSession(channel_name), nullptr);
}

TEST_F(GpuChannelManagerTest, PublicTmpPathCannotPreemptPrivateLock) {
  auto* manager = GpuChannelManager::Instance();
  const std::string channel = UniqueChannel();
  const std::string old_path = "/tmp/cyber_gpu_writer_" +
                               std::to_string(::geteuid()) + "_" +
                               std::to_string(common::Hash(channel)) + ".lock";
  ASSERT_EQ(::symlink(runtime_dir_.c_str(), old_path.c_str()), 0);
  cleanup_files_.push_back(old_path);

  auto session = NewSession(channel);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(manager->ClaimWriterSession(channel, session));
  WriterClaimGuard claim{manager, channel, session->session_id()};
  EXPECT_TRUE(RunLockChildProcess(channel, "0"));
}

TEST_F(GpuChannelManagerTest, RejectsUnsafeRuntimeDirectoryAndLockFile) {
  auto* manager = GpuChannelManager::Instance();
  const std::string channel = UniqueChannel();
  auto session = NewSession(channel);
  ASSERT_NE(session, nullptr);

  ASSERT_EQ(::chmod(runtime_dir_.c_str(), 0777), 0);
  EXPECT_FALSE(manager->ClaimWriterSession(channel, session));
  ASSERT_EQ(::chmod(runtime_dir_.c_str(), 0700), 0);

  const std::string lock_dir = runtime_dir_ + "/cyber_gpu_writer";
  ASSERT_EQ(::symlink(runtime_dir_.c_str(), lock_dir.c_str()), 0);
  EXPECT_FALSE(manager->ClaimWriterSession(channel, session));
  ASSERT_EQ(::unlink(lock_dir.c_str()), 0);
  ASSERT_EQ(::mkdir(lock_dir.c_str(), 0700), 0);
  const std::string lock_path = cleanup_files_.front();
  ASSERT_EQ(::symlink(runtime_dir_.c_str(), lock_path.c_str()), 0);
  EXPECT_FALSE(manager->ClaimWriterSession(channel, session));
  ASSERT_EQ(::unlink(lock_path.c_str()), 0);

  ASSERT_EQ(::chmod(lock_dir.c_str(), 0777), 0);
  EXPECT_FALSE(manager->ClaimWriterSession(channel, session));
  ASSERT_EQ(::chmod(lock_dir.c_str(), 0700), 0);
  ASSERT_TRUE(manager->ClaimWriterSession(channel, session));
  WriterClaimGuard claim{manager, channel, session->session_id()};
}

TEST_F(GpuChannelManagerTest, HomeFallbackUsesPrivateDirectory) {
  const std::string channel = UniqueChannel();
  auto session = NewSession(channel);
  ASSERT_NE(session, nullptr);
  ASSERT_EQ(::setenv("HOME", runtime_dir_.c_str(), 1), 0);
  ASSERT_EQ(::unsetenv("XDG_RUNTIME_DIR"), 0);
  auto* manager = GpuChannelManager::Instance();
  ASSERT_TRUE(manager->ClaimWriterSession(channel, session));
  WriterClaimGuard claim{manager, channel, session->session_id()};
}

TEST_F(GpuChannelManagerTest, WrongReleaseAndRemovalRespectWriterLifetime) {
  auto* manager = GpuChannelManager::Instance();
  const std::string channel = UniqueChannel();
  auto session = NewSession(channel);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(manager->ClaimWriterSession(channel, session));
  manager->ReleaseWriterSession(channel, session->session_id() + 1);
  EXPECT_TRUE(RunLockChildProcess(channel, "0"));
  manager->RemoveSession(channel);
  EXPECT_TRUE(RunLockChildProcess(channel, "1"));
  session = NewSession(channel);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(manager->ClaimWriterSession(channel, session));
  manager->Clear();
  EXPECT_TRUE(RunLockChildProcess(channel, "1"));
}

TEST_F(GpuChannelManagerTest, ProcessExitReleasesWriterLock) {
  auto* manager = GpuChannelManager::Instance();
  const std::string channel = UniqueChannel();
  EXPECT_TRUE(RunLockChildProcess(channel, "exit"));
  auto session = NewSession(channel);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(manager->ClaimWriterSession(channel, session));
  WriterClaimGuard claim{manager, channel, session->session_id()};
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  if (std::getenv("CYBER_GPU_WRITER_LOCK_CHILD_CHANNEL") != nullptr) {
    return apollo::cyber::transport::RunLockChild();
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

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

#include "cyber/transport/nvsci/nvsci_sync_engine.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

namespace apollo {
namespace cyber {
namespace transport {
namespace {

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (size != 0) {
    const ssize_t written = write(fd, bytes, size);
    if (written <= 0) {
      return false;
    }
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool ReadAll(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  while (size != 0) {
    const ssize_t count = read(fd, bytes, size);
    if (count <= 0) {
      return false;
    }
    bytes += count;
    size -= static_cast<size_t>(count);
  }
  return true;
}

}  // namespace

int RunCudaEventChildFromEnvironment() {
  const char* pipe_fd_text = std::getenv("GPU_SYNC_EVENT_CHILD_FD");
  if (pipe_fd_text == nullptr) {
    return 20;
  }
  const int pipe_fd = std::atoi(pipe_fd_text);
  uint8_t enabled = 0;
  if (!ReadAll(pipe_fd, &enabled, sizeof(enabled)) || enabled == 0) {
    return 0;
  }
  uint32_t sync_size = 0;
  cudaIpcMemHandle_t mem_handle{};
  NvSciSyncFence fence;
  if (!ReadAll(pipe_fd, &sync_size, sizeof(sync_size)) || sync_size == 0 ||
      sync_size > 4096) {
    return 10;
  }
  std::vector<uint8_t> sync_desc(sync_size);
  if (!ReadAll(pipe_fd, sync_desc.data(), sync_desc.size()) ||
      !ReadAll(pipe_fd, &mem_handle, sizeof(mem_handle)) ||
      !ReadAll(pipe_fd, &fence, sizeof(fence))) {
    return 11;
  }
  void* imported = nullptr;
  cudaStream_t stream = nullptr;
  NvSciSyncEngine peer(202);
  if (!peer.ImportSyncObj(sync_desc)) {
    return 12;
  }
  if (cudaIpcOpenMemHandle(&imported, mem_handle,
                           cudaIpcMemLazyEnablePeerAccess) != cudaSuccess) {
    return 13;
  }
  if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) !=
      cudaSuccess) {
    return 14;
  }
  if (!peer.InsertWaitFence(stream, fence)) {
    return 15;
  }
  uint8_t value = 0;
  const bool ok = cudaMemcpyAsync(&value, imported, 1, cudaMemcpyDeviceToHost,
                                  stream) == cudaSuccess &&
                  cudaStreamSynchronize(stream) == cudaSuccess &&
                  value == 0x7b;
  cudaStreamDestroy(stream);
  cudaIpcCloseMemHandle(imported);
  return ok ? 0 : 16;
}

TEST(NvSciSyncEngineTest, FenceGenerationAndValidation) {
  NvSciSyncEngine engine(42);

  NvSciSyncFence f1 = engine.GenerateSignalFence(nullptr);
  NvSciSyncFence f2 = engine.GenerateSignalFence(nullptr);

  EXPECT_TRUE(f1.IsValid());
  EXPECT_TRUE(f2.IsValid());
  EXPECT_NE(f1.fence_id, f2.fence_id);
  EXPECT_LE(f1.timestamp_ns, f2.timestamp_ns);

  // Wait insertion
  EXPECT_TRUE(engine.InsertWaitFence(nullptr, f1));
  NvSciSyncFence invalid_fence;
  EXPECT_FALSE(engine.InsertWaitFence(nullptr, invalid_fence));

  // Signal check
  EXPECT_FALSE(engine.IsFenceSignaled(f1));
  engine.MarkFenceCompleted(f1.fence_id);
  EXPECT_TRUE(engine.IsFenceSignaled(f1));
}

TEST(NvSciSyncEngineTest, ExportAndImport) {
  NvSciSyncEngine engine(100);
  std::vector<uint8_t> corrupted_desc = {1, 2, 3};
  EXPECT_FALSE(engine.ImportSyncObj(corrupted_desc));
}

TEST(NvSciSyncEngineTest, DoesNotReusePendingCudaEvents) {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    GTEST_SKIP() << "A CUDA device is required for event ring validation.";
  }

  cudaStream_t blocker_stream = nullptr;
  cudaStream_t signal_stream = nullptr;
  cudaEvent_t blocker = nullptr;
  ASSERT_EQ(cudaStreamCreateWithFlags(&blocker_stream, cudaStreamNonBlocking),
            cudaSuccess);
  ASSERT_EQ(cudaStreamCreateWithFlags(&signal_stream, cudaStreamNonBlocking),
            cudaSuccess);
  ASSERT_EQ(cudaEventCreateWithFlags(&blocker, cudaEventDisableTiming),
            cudaSuccess);
  ASSERT_EQ(cudaLaunchHostFunc(
                blocker_stream,
                [](void*) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
                },
                nullptr),
            cudaSuccess);
  ASSERT_EQ(cudaEventRecord(blocker, blocker_stream), cudaSuccess);
  ASSERT_EQ(cudaStreamWaitEvent(signal_stream, blocker, 0), cudaSuccess);

  NvSciSyncEngine engine(43);
  for (size_t index = 0; index < 16; ++index) {
    EXPECT_TRUE(engine.GenerateSignalFence(signal_stream).IsValid());
  }
  EXPECT_FALSE(engine.GenerateSignalFence(signal_stream).IsValid());

  ASSERT_EQ(cudaStreamSynchronize(signal_stream), cudaSuccess);
  EXPECT_TRUE(engine.GenerateSignalFence(signal_stream).IsValid());
  ASSERT_EQ(cudaStreamSynchronize(signal_stream), cudaSuccess);
  EXPECT_EQ(cudaEventDestroy(blocker), cudaSuccess);
  EXPECT_EQ(cudaStreamDestroy(signal_stream), cudaSuccess);
  EXPECT_EQ(cudaStreamDestroy(blocker_stream), cudaSuccess);
}

TEST(NvSciSyncEngineTest, CudaEventSynchronizesAcrossProcesses) {
  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);
  // Fork before either process initializes CUDA. In production the peers are
  // independently exec'ed; inheriting an initialized CUDA runtime is invalid.
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(pipe_fds[1]);
    const std::string pipe_fd = std::to_string(pipe_fds[0]);
    setenv("GPU_SYNC_EVENT_CHILD_FD", pipe_fd.c_str(), 1);
    execl("/proc/self/exe", "/proc/self/exe", nullptr);
    _exit(21);
  }

  close(pipe_fds[0]);

  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    const uint8_t disabled = 0;
    ASSERT_TRUE(WriteAll(pipe_fds[1], &disabled, sizeof(disabled)));
    close(pipe_fds[1]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    GTEST_SKIP() << "A CUDA device is required for CUDA IPC event validation.";
  }

  void* device_ptr = nullptr;
  ASSERT_EQ(cudaMalloc(&device_ptr, 4096), cudaSuccess);
  cudaStream_t stream = nullptr;
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
            cudaSuccess);
  cudaIpcMemHandle_t mem_handle{};
  ASSERT_EQ(cudaIpcGetMemHandle(&mem_handle, device_ptr), cudaSuccess);

  NvSciSyncEngine engine(101);
  std::vector<uint8_t> sync_desc;
  ASSERT_TRUE(engine.ExportSyncObj(&sync_desc));
  ASSERT_EQ(cudaMemsetAsync(device_ptr, 0x7b, 4096, stream), cudaSuccess);
  const NvSciSyncFence fence = engine.GenerateSignalFence(stream);
  ASSERT_TRUE(fence.IsValid());

  const uint8_t enabled = 1;
  const uint32_t sync_size = static_cast<uint32_t>(sync_desc.size());
  ASSERT_TRUE(WriteAll(pipe_fds[1], &enabled, sizeof(enabled)));
  ASSERT_TRUE(WriteAll(pipe_fds[1], &sync_size, sizeof(sync_size)));
  ASSERT_TRUE(WriteAll(pipe_fds[1], sync_desc.data(), sync_desc.size()));
  ASSERT_TRUE(WriteAll(pipe_fds[1], &mem_handle, sizeof(mem_handle)));
  ASSERT_TRUE(WriteAll(pipe_fds[1], &fence, sizeof(fence)));
  close(pipe_fds[1]);

  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  cudaStreamDestroy(stream);
  cudaFree(device_ptr);
}

TEST(NvSciSyncEngineTest, ExportsVersionedCudaDescriptor) {
  NvSciSyncEngine engine(100);
  std::vector<uint8_t> sync_desc;
  if (!engine.ExportSyncObj(&sync_desc)) {
    GTEST_SKIP() << "CUDA IPC events are unavailable on this host.";
  }
  EXPECT_GE(sync_desc.size(), 32U);
  EXPECT_EQ(sync_desc[0], 0x47);
  EXPECT_EQ(sync_desc[1], 0x50);
  EXPECT_EQ(sync_desc[2], 0x59);
  EXPECT_EQ(sync_desc[3], 0x53);
  EXPECT_TRUE(engine.ImportSyncObj(sync_desc));
}

TEST(NvSciSyncEngineTest, HostSynchronizedFenceCompletesBeforeHandoff) {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    GTEST_SKIP() << "A CUDA device is required for host synchronization.";
  }

  cudaStream_t stream = nullptr;
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
            cudaSuccess);
  void* device_ptr = nullptr;
  ASSERT_EQ(cudaMalloc(&device_ptr, 4096), cudaSuccess);
  ASSERT_EQ(cudaMemsetAsync(device_ptr, 0x6d, 4096, stream), cudaSuccess);

  NvSciSyncEngine producer(301, true);
  const NvSciSyncFence fence = producer.GenerateSignalFence(stream);
  ASSERT_TRUE(fence.IsValid());

  uint8_t value = 0;
  ASSERT_EQ(cudaMemcpy(&value, device_ptr, sizeof(value),
                       cudaMemcpyDeviceToHost),
            cudaSuccess);
  EXPECT_EQ(value, 0x6d);

  std::vector<uint8_t> descriptor;
  ASSERT_TRUE(producer.ExportSyncObj(&descriptor));
  NvSciSyncEngine consumer(302, true);
  EXPECT_TRUE(consumer.ImportSyncObj(descriptor));
  EXPECT_TRUE(consumer.InsertWaitFence(stream, fence));
  EXPECT_TRUE(consumer.IsFenceSignaled(fence));

  cudaFree(device_ptr);
  cudaStreamDestroy(stream);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  if (std::getenv("GPU_SYNC_EVENT_CHILD_FD") != nullptr) {
    return apollo::cyber::transport::RunCudaEventChildFromEnvironment();
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

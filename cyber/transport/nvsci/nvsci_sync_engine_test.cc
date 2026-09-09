/******************************************************************************
 * Copyright 2026 WheelOS. All Rights Reserved.
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

#include "cyber/transport/nvsci/nvsci_sync_engine.h"

#include <sys/wait.h>
#include <unistd.h>

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

TEST(NvSciSyncEngineTest, CudaEventSynchronizesAcrossProcesses) {
  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(pipe_fds[1]);
    uint8_t enabled = 0;
    if (!ReadAll(pipe_fds[0], &enabled, sizeof(enabled)) || enabled == 0) {
      _exit(0);
    }
    uint32_t sync_size = 0;
    cudaIpcMemHandle_t mem_handle{};
    NvSciSyncFence fence;
    if (!ReadAll(pipe_fds[0], &sync_size, sizeof(sync_size)) ||
        sync_size == 0 || sync_size > 4096) {
      _exit(10);
    }
    std::vector<uint8_t> sync_desc(sync_size);
    if (!ReadAll(pipe_fds[0], sync_desc.data(), sync_desc.size()) ||
        !ReadAll(pipe_fds[0], &mem_handle, sizeof(mem_handle)) ||
        !ReadAll(pipe_fds[0], &fence, sizeof(fence))) {
      _exit(11);
    }
    void* imported = nullptr;
    cudaStream_t stream = nullptr;
    NvSciSyncEngine peer(202);
    if (!peer.ImportSyncObj(sync_desc)) {
      fprintf(stderr, "cuda event import failed: %s\n",
              cudaGetErrorString(cudaGetLastError()));
      _exit(12);
    }
    if (cudaIpcOpenMemHandle(&imported, mem_handle,
                             cudaIpcMemLazyEnablePeerAccess) != cudaSuccess) {
      _exit(13);
    }
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) !=
        cudaSuccess) {
      _exit(14);
    }
    if (!peer.InsertWaitFence(stream, fence)) {
      _exit(15);
    }
    uint8_t value = 0;
    const bool ok = cudaMemcpyAsync(&value, imported, 1, cudaMemcpyDeviceToHost,
                                    stream) == cudaSuccess &&
                    cudaStreamSynchronize(stream) == cudaSuccess &&
                    value == 0x7b;
    cudaStreamDestroy(stream);
    cudaIpcCloseMemHandle(imported);
    _exit(ok ? 0 : 16);
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
  cudaStream_t stream = nullptr;
  ASSERT_EQ(cudaMalloc(&device_ptr, 4096), cudaSuccess);
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

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

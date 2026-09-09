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

#include <cstdint>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "cyber/transport/nvsci/nvsci_buf_pool.h"

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
    const ssize_t read_bytes = read(fd, bytes, size);
    if (read_bytes <= 0) {
      return false;
    }
    bytes += read_bytes;
    size -= static_cast<size_t>(read_bytes);
  }
  return true;
}

int RunChild(int control_fd, int descriptor_fd) {
  uint8_t enabled = 0;
  if (!ReadAll(control_fd, &enabled, sizeof(enabled)) || enabled == 0) {
    return 0;
  }

  uint32_t descriptor_size = 0;
  if (!ReadAll(descriptor_fd, &descriptor_size, sizeof(descriptor_size)) ||
      descriptor_size == 0 || descriptor_size > 4096) {
    return 10;
  }
  std::vector<uint8_t> descriptor(descriptor_size);
  if (!ReadAll(descriptor_fd, descriptor.data(), descriptor.size())) {
    return 11;
  }

  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 4096;
  auto pool = std::make_shared<NvSciBufPool>(config);
  if (!pool->Initialize() || !pool->ImportBuffer(0, descriptor)) {
    return 12;
  }

  uint8_t value = 0;
  if (cudaMemcpy(&value, pool->GetDevicePtr(0), sizeof(value),
                 cudaMemcpyDeviceToHost) != cudaSuccess) {
    return 13;
  }
  return value == 0x5a ? 0 : 14;
}

}  // namespace

TEST(CudaIpcProcessTest, ExportImportAndReadAcrossProcesses) {
  int control_pipe[2];
  int descriptor_pipe[2];
  ASSERT_EQ(pipe(control_pipe), 0);
  ASSERT_EQ(pipe(descriptor_pipe), 0);

  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(control_pipe[1]);
    close(descriptor_pipe[1]);
    const int result = RunChild(control_pipe[0], descriptor_pipe[0]);
    close(control_pipe[0]);
    close(descriptor_pipe[0]);
    _exit(result);
  }

  close(control_pipe[0]);
  close(descriptor_pipe[0]);

  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    const uint8_t disabled = 0;
    ASSERT_TRUE(WriteAll(control_pipe[1], &disabled, sizeof(disabled)));
    close(control_pipe[1]);
    close(descriptor_pipe[1]);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    GTEST_SKIP() << "A CUDA device is required for CUDA IPC validation.";
  }

  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 4096;
  auto pool = std::make_shared<NvSciBufPool>(config);
  ASSERT_TRUE(pool->Initialize());
  ASSERT_EQ(cudaMemset(pool->GetDevicePtr(0), 0x5a, config.slot_size),
            cudaSuccess);

  std::vector<uint8_t> descriptor;
  ASSERT_TRUE(pool->ExportBuffer(0, &descriptor));
  ASSERT_LE(descriptor.size(), 4096U);
  const uint8_t enabled = 1;
  ASSERT_TRUE(WriteAll(control_pipe[1], &enabled, sizeof(enabled)));
  const uint32_t descriptor_size = static_cast<uint32_t>(descriptor.size());
  ASSERT_TRUE(WriteAll(descriptor_pipe[1], &descriptor_size,
                       sizeof(descriptor_size)));
  ASSERT_TRUE(WriteAll(descriptor_pipe[1], descriptor.data(), descriptor.size()));
  close(control_pipe[1]);
  close(descriptor_pipe[1]);

  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(CudaIpcProcessTest, MalformedControlDescriptorIsRejected) {
  int control_pipe[2];
  int descriptor_pipe[2];
  ASSERT_EQ(pipe(control_pipe), 0);
  ASSERT_EQ(pipe(descriptor_pipe), 0);

  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(control_pipe[1]);
    close(descriptor_pipe[1]);
    const int result = RunChild(control_pipe[0], descriptor_pipe[0]);
    close(control_pipe[0]);
    close(descriptor_pipe[0]);
    _exit(result);
  }

  close(control_pipe[0]);
  close(descriptor_pipe[0]);
  const uint8_t enabled = 1;
  ASSERT_TRUE(WriteAll(control_pipe[1], &enabled, sizeof(enabled)));
  const uint32_t malformed_size = 0;
  ASSERT_TRUE(WriteAll(descriptor_pipe[1], &malformed_size,
                       sizeof(malformed_size)));
  close(control_pipe[1]);
  close(descriptor_pipe[1]);

  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 10);
}

TEST(CudaIpcProcessTest, InvalidDescriptorMagicIsRejected) {
  int control_pipe[2];
  int descriptor_pipe[2];
  ASSERT_EQ(pipe(control_pipe), 0);
  ASSERT_EQ(pipe(descriptor_pipe), 0);

  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(control_pipe[1]);
    close(descriptor_pipe[1]);
    const int result = RunChild(control_pipe[0], descriptor_pipe[0]);
    close(control_pipe[0]);
    close(descriptor_pipe[0]);
    _exit(result);
  }

  close(control_pipe[0]);
  close(descriptor_pipe[0]);
  const uint8_t enabled = 1;
  ASSERT_TRUE(WriteAll(control_pipe[1], &enabled, sizeof(enabled)));
  std::vector<uint8_t> malformed_descriptor(20, 0);
  const uint32_t descriptor_size =
      static_cast<uint32_t>(malformed_descriptor.size());
  ASSERT_TRUE(WriteAll(descriptor_pipe[1], &descriptor_size,
                       sizeof(descriptor_size)));
  ASSERT_TRUE(WriteAll(descriptor_pipe[1], malformed_descriptor.data(),
                       malformed_descriptor.size()));
  close(control_pipe[1]);
  close(descriptor_pipe[1]);

  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 12);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

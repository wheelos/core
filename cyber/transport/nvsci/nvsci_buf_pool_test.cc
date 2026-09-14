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

#include "cyber/transport/nvsci/nvsci_buf_pool.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

namespace apollo {
namespace cyber {
namespace transport {

TEST(NvSciBufPoolTest, InitializationAndAcquisition) {
  NvSciBufPoolConfig config;
  config.slot_count = 3;
  config.slot_size = 1024 * 1024;
  config.alignment = 4096;

  NvSciBufPool pool(config);
  EXPECT_TRUE(pool.Initialize());
  EXPECT_EQ(pool.GetSlotCount(), 3);
  EXPECT_EQ(pool.GetSlotCapacity(), 1024 * 1024);

  int slot0 = pool.AcquireSlot();
  int slot1 = pool.AcquireSlot();
  int slot2 = pool.AcquireSlot();

  EXPECT_EQ(slot0, 0);
  EXPECT_EQ(slot1, 1);
  EXPECT_EQ(slot2, 2);

  // All slots in use, next acquire must return -1
  EXPECT_EQ(pool.AcquireSlot(), -1);

  EXPECT_EQ(pool.GetSlotState(0), SlotState::LOANED);
  EXPECT_EQ(pool.GetSlotState(1), SlotState::LOANED);
  EXPECT_EQ(pool.GetSlotState(2), SlotState::LOANED);

  // Check alignment
  void* ptr0 = pool.GetDevicePtr(0);
  EXPECT_NE(ptr0, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr0) % config.alignment, 0);

  // Mark in use
  EXPECT_TRUE(pool.MarkInUse(0, 2));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::IN_USE);

  // Release first ref
  NvSciSyncFence fence;
  fence.fence_id = 123;
  EXPECT_TRUE(pool.ReleaseSlot(0, fence));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::IN_USE);

  // Release second ref -> should become FREE
  EXPECT_TRUE(pool.ReleaseSlot(0, fence));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::FREE);

  // Now we can acquire again
  int slot_reacquired = pool.AcquireSlot();
  EXPECT_EQ(slot_reacquired, 0);
}

TEST(NvSciBufPoolTest, ExportAndImport) {
  NvSciBufPoolConfig config;
  config.slot_count = 2;
  config.slot_size = 512 * 1024;

  NvSciBufPool pool(config);
  EXPECT_TRUE(pool.Initialize());

  std::vector<uint8_t> ipc_desc;
  EXPECT_TRUE(pool.ExportBuffer(1, &ipc_desc));
  EXPECT_FALSE(ipc_desc.empty());

  EXPECT_TRUE(pool.ImportBuffer(1, ipc_desc));
  // Mismatched slot id import should fail
  EXPECT_FALSE(pool.ImportBuffer(0, ipc_desc));
}

TEST(NvSciBufPoolTest, OrinSharedMemoryIsPrivateAndUnlinkedOnDestruction) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 4096;
  config.force_uma_shm = true;

  std::string shm_name;
  {
    NvSciBufPool pool(config);
    ASSERT_TRUE(pool.Initialize());
    std::vector<uint8_t> descriptor;
    ASSERT_TRUE(pool.ExportBuffer(0, &descriptor));
    ASSERT_GE(descriptor.size(), kGpuBufferDescriptorHeaderSize);

    uint32_t magic = 0;
    uint32_t name_size = 0;
    std::memcpy(&magic, descriptor.data(), sizeof(magic));
    ASSERT_EQ(magic, kOrinUmaBufferMagic);
    std::memcpy(&name_size, descriptor.data() + 12, sizeof(name_size));
    ASSERT_EQ(descriptor.size(), kGpuBufferDescriptorHeaderSize + name_size);
    shm_name.assign(
        reinterpret_cast<const char*>(descriptor.data() +
                                      kGpuBufferDescriptorHeaderSize),
        name_size);

    const int fd = shm_open(shm_name.c_str(), O_RDWR | O_CLOEXEC, 0);
    ASSERT_GE(fd, 0);
    struct stat shm_stat {};
    ASSERT_EQ(fstat(fd, &shm_stat), 0);
    EXPECT_EQ(shm_stat.st_size, static_cast<off_t>(config.slot_size));
    EXPECT_EQ(shm_stat.st_mode & 0777, 0600);
    close(fd);
  }

  errno = 0;
  EXPECT_EQ(shm_open(shm_name.c_str(), O_RDWR | O_CLOEXEC, 0), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST(NvSciBufPoolTest, UmaSharedMemoryIsGpuAndCpuCoherent) {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    GTEST_SKIP() << "A CUDA device is required for mapped shared memory.";
  }

  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 4096;
  config.force_uma_shm = true;
  NvSciBufPool pool(config);
  ASSERT_TRUE(pool.Initialize());
  ASSERT_EQ(pool.GetBackend(), GpuBufferBackend::ORIN_UMA);

  std::vector<uint8_t> descriptor;
  ASSERT_TRUE(pool.ExportBuffer(0, &descriptor));
  uint32_t name_size = 0;
  std::memcpy(&name_size, descriptor.data() + 12, sizeof(name_size));
  const std::string shm_name(
      reinterpret_cast<const char*>(descriptor.data() +
                                    kGpuBufferDescriptorHeaderSize),
      name_size);
  const int fd = shm_open(shm_name.c_str(), O_RDWR | O_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  void* mapping =
      mmap(nullptr, config.slot_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ASSERT_NE(mapping, MAP_FAILED);

  ASSERT_EQ(cudaMemset(pool.GetDevicePtr(0), 0x4c, config.slot_size),
            cudaSuccess);
  EXPECT_EQ(static_cast<uint8_t*>(mapping)[config.slot_size - 1], 0x4c);

  static_cast<uint8_t*>(mapping)[0] = 0x7e;
  uint8_t value = 0;
  ASSERT_EQ(cudaMemcpy(&value, pool.GetDevicePtr(0), sizeof(value),
                       cudaMemcpyDeviceToHost),
            cudaSuccess);
  EXPECT_EQ(value, 0x7e);

  munmap(mapping, config.slot_size);
  close(fd);
}

TEST(NvSciBufPoolTest, QuarantineAndUnquarantine) {
  NvSciBufPoolConfig config;
  config.slot_count = 2;
  config.slot_size = 1024;

  NvSciBufPool pool(config);
  EXPECT_TRUE(pool.Initialize());

  int slot = pool.AcquireSlot();
  EXPECT_EQ(slot, 0);
  EXPECT_TRUE(pool.MarkInUse(0, 1));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::IN_USE);

  EXPECT_TRUE(pool.QuarantineSlot(0));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::QUARANTINED);

  // While quarantined, AcquireSlot must not hand out slot 0
  int next_slot = pool.AcquireSlot();
  EXPECT_EQ(next_slot, 1);
  EXPECT_EQ(pool.AcquireSlot(), -1);

  // Unquarantine restores it to FREE
  EXPECT_TRUE(pool.UnquarantineSlot(0));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::FREE);
  EXPECT_EQ(pool.AcquireSlot(), 0);
}

TEST(NvSciBufPoolTest, RejectsInvalidReferenceTransitions) {
  NvSciBufPoolConfig config;
  config.slot_count = 1;
  config.slot_size = 1024;
  NvSciBufPool pool(config);
  ASSERT_TRUE(pool.Initialize());

  EXPECT_EQ(pool.AcquireSlot(), 0);
  EXPECT_FALSE(pool.MarkInUse(0, 0));
  EXPECT_TRUE(pool.MarkInUse(0, 1));
  EXPECT_TRUE(pool.QuarantineSlot(0));

  NvSciSyncFence fence;
  fence.fence_id = 7;
  EXPECT_FALSE(pool.ReleaseSlot(0, fence));
  EXPECT_EQ(pool.GetSlotState(0), SlotState::QUARANTINED);
  EXPECT_TRUE(pool.UnquarantineSlot(0));
  EXPECT_FALSE(pool.ReleaseSlot(0, fence));
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

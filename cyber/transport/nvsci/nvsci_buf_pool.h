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

#ifndef CYBER_TRANSPORT_NVSCI_NVSCI_BUF_POOL_H_
#define CYBER_TRANSPORT_NVSCI_NVSCI_BUF_POOL_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "cyber/common/log.h"
#include "cyber/transport/nvsci/nvsci_types.h"

namespace apollo {
namespace cyber {
namespace transport {

class INvSciBufPool {
 public:
  virtual ~INvSciBufPool() = default;

  virtual int AcquireSlot() = 0;
  virtual bool MarkInUse(int slot_id, int32_t ref_count) = 0;
  virtual bool ReleaseSlot(int slot_id, const NvSciSyncFence& post_fence) = 0;
  virtual void* GetDevicePtr(int slot_id) = 0;
  virtual size_t GetSlotCount() const = 0;
  virtual size_t GetSlotCapacity() const = 0;
  virtual uint32_t GetAlignment() const = 0;
  virtual SlotState GetSlotState(int slot_id) const = 0;
  virtual bool ExportBuffer(int slot_id, std::vector<uint8_t>* ipc_desc) = 0;
  virtual bool ImportBuffer(int slot_id,
                            const std::vector<uint8_t>& ipc_desc) = 0;
  virtual NvSciSyncFence GetLastPostFence(int slot_id) const = 0;
  virtual std::vector<NvSciSyncFence> GetLastPostFences(int slot_id) const = 0;
  virtual void AddPostFence(int slot_id, const NvSciSyncFence& post_fence) = 0;
  virtual void ClearPostFences(int slot_id) = 0;
  virtual void ClearPostFencesForEngine(uint64_t engine_id) = 0;
  virtual bool QuarantineSlot(int slot_id) = 0;
  virtual bool UnquarantineSlot(int slot_id) = 0;
};

class NvSciBufPool : public INvSciBufPool {
 public:
  explicit NvSciBufPool(const NvSciBufPoolConfig& config);
  ~NvSciBufPool() override;

  bool Initialize();

  int AcquireSlot() override;
  bool MarkInUse(int slot_id, int32_t ref_count) override;
  bool ReleaseSlot(int slot_id, const NvSciSyncFence& post_fence) override;
  void* GetDevicePtr(int slot_id) override;
  size_t GetSlotCount() const override { return config_.slot_count; }
  size_t GetSlotCapacity() const override { return config_.slot_size; }
  uint32_t GetAlignment() const override { return config_.alignment; }
  SlotState GetSlotState(int slot_id) const override;
  bool ExportBuffer(int slot_id, std::vector<uint8_t>* ipc_desc) override;
  bool ImportBuffer(int slot_id, const std::vector<uint8_t>& ipc_desc) override;
  bool ImportBufferStrict(int slot_id, const std::vector<uint8_t>& ipc_desc);
  NvSciSyncFence GetLastPostFence(int slot_id) const override;
  std::vector<NvSciSyncFence> GetLastPostFences(int slot_id) const override;
  void AddPostFence(int slot_id, const NvSciSyncFence& post_fence) override;
  void ClearPostFences(int slot_id) override;
  void ClearPostFencesForEngine(uint64_t engine_id) override;
  bool QuarantineSlot(int slot_id) override;
  bool UnquarantineSlot(int slot_id) override;

 private:
  struct SlotEntry {
    int slot_id = -1;
    std::atomic<SlotState> state{SlotState::FREE};
    std::atomic<int32_t> ref_count{0};
    void* dev_ptr = nullptr;
    void* host_shm_ptr = nullptr;
    std::string shm_name;
    int shm_fd = -1;
    bool is_shm_creator = false;
    bool is_cuda_allocated = false;
    bool is_ipc_opened = false;
    std::vector<uint8_t> allocated_storage;
    std::vector<NvSciSyncFence> last_post_fences;
  };

  NvSciBufPoolConfig config_;
  uint64_t pool_uid_ = 0;
  std::vector<std::unique_ptr<SlotEntry>> slots_;
  mutable std::mutex mutex_;
  uint32_t next_slot_hint_ = 0;
  bool is_initialized_ = false;

  bool ImportBufferInternal(int slot_id, const std::vector<uint8_t>& ipc_desc,
                            bool strict);
};

using NvSciBufPoolPtr = std::shared_ptr<INvSciBufPool>;

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_NVSCI_BUF_POOL_H_

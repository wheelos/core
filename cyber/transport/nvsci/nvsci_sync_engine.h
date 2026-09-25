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

#ifndef CYBER_TRANSPORT_NVSCI_NVSCI_SYNC_ENGINE_H_
#define CYBER_TRANSPORT_NVSCI_NVSCI_SYNC_ENGINE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(CYBER_USE_CUDA_IPC)
#include <cuda_runtime.h>
#endif

#include "cyber/transport/nvsci/nvsci_types.h"

namespace apollo {
namespace cyber {
namespace transport {

class INvSciSyncEngine {
 public:
  virtual ~INvSciSyncEngine() = default;

  /**
   * @brief Generates an asynchronous hardware signal fence on the given CUDA
   * stream.
   *
   * In DriveOS, this calls cudaSignalExternalSemaphoresAsync and
   * NvSciSyncObjGenerateFence. CPU does not wait; fence is ready immediately to
   * be passed downstream.
   */
  virtual NvSciSyncFence GenerateSignalFence(void* stream_ptr) = 0;

  /**
   * @brief Inserts an asynchronous wait on the given CUDA stream.
   *
   * In DriveOS, this calls cudaWaitExternalSemaphoresAsync.
   * The downstream stream will wait until the hardware fence triggers, without
   * CPU blocking.
   */
  virtual bool InsertWaitFence(void* stream_ptr,
                               const NvSciSyncFence& fence) = 0;

  /**
   * @brief Exports the sync object descriptor for one-time channel negotiation.
   */
  virtual bool ExportSyncObj(std::vector<uint8_t>* sync_desc) = 0;

  /**
   * @brief Imports the peer sync object descriptor.
   */
  virtual bool ImportSyncObj(const std::vector<uint8_t>& sync_desc) = 0;

  /**
   * @brief Releases a previously imported peer synchronization object.
   */
  virtual void ReleaseSyncObj(const std::vector<uint8_t>& sync_desc) = 0;
  virtual bool GetSyncObjId(const std::vector<uint8_t>& sync_desc,
                            uint64_t* engine_id) const = 0;

  /**
   * @brief Non-blocking status query for a given fence.
   */
  virtual bool IsFenceSignaled(const NvSciSyncFence& fence) = 0;

  /**
   * @brief Records completion for a synchronous CPU-side fence.
   */
  virtual void MarkFenceCompleted(uint64_t fence_id) = 0;
};

class NvSciSyncEngine : public INvSciSyncEngine {
 public:
  NvSciSyncEngine();
  explicit NvSciSyncEngine(uint64_t engine_id, bool host_synchronized = false);
  ~NvSciSyncEngine() override;

  NvSciSyncFence GenerateSignalFence(void* stream_ptr) override;
  bool InsertWaitFence(void* stream_ptr, const NvSciSyncFence& fence) override;
  bool ExportSyncObj(std::vector<uint8_t>* sync_desc) override;
  bool ImportSyncObj(const std::vector<uint8_t>& sync_desc) override;
  void ReleaseSyncObj(const std::vector<uint8_t>& sync_desc) override;
  bool GetSyncObjId(const std::vector<uint8_t>& sync_desc,
                    uint64_t* engine_id) const override;
  bool IsFenceSignaled(const NvSciSyncFence& fence) override;

  void MarkFenceCompleted(uint64_t fence_id) override;
  void PruneSignaledEvents();

 private:
  static constexpr size_t kEventRingSize = 16;
  uint64_t engine_id_ = 0;
  std::atomic<uint64_t> fence_seq_{1};
  mutable std::mutex mutex_;
  std::unordered_set<uint64_t> completed_fences_;
  std::unordered_set<uint64_t> pending_streamless_fences_;
  std::unordered_set<uint64_t> imported_host_engines_;
  bool host_synchronized_ = false;
#if defined(CYBER_USE_CUDA_IPC)
  bool EnsureCudaIpcEvents();
  std::array<cudaEvent_t, kEventRingSize> cuda_events_{};
  std::array<bool, kEventRingSize> event_recorded_{};
  std::unordered_map<uint64_t, std::vector<cudaEvent_t>> imported_cuda_events_;
#endif
};

using NvSciSyncEnginePtr = std::shared_ptr<INvSciSyncEngine>;

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_NVSCI_SYNC_ENGINE_H_

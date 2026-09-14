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

#include <chrono>
#include <cstring>

#include "cyber/transport/nvsci/gpu_wire.h"
#include "cyber/transport/nvsci/orin_nvsci_backend.h"

namespace apollo {
namespace cyber {
namespace transport {
namespace {

constexpr uint32_t kSyncDescriptorMagic = 0x53595047;  // "GPYS"
constexpr uint32_t kSyncDescriptorVersion = 1;
constexpr uint32_t kSyncBackendCudaIpc = 1;
constexpr uint32_t kSyncBackendHostSynchronized = 2;
constexpr uint32_t kFencePayloadMagic = 0x434E5953;  // "SYNC"
constexpr uint8_t kHostSynchronizedFence = 1;

void StoreFenceOwner(uint64_t engine_id, uint32_t event_idx,
                     NvSciSyncFence* fence) {
  fence->payload[0] = static_cast<uint8_t>(kFencePayloadMagic);
  fence->payload[1] = static_cast<uint8_t>(kFencePayloadMagic >> 8);
  fence->payload[2] = static_cast<uint8_t>(kFencePayloadMagic >> 16);
  fence->payload[3] = static_cast<uint8_t>(kFencePayloadMagic >> 24);
  fence->payload[4] = static_cast<uint8_t>(event_idx & 0xffU);
  fence->payload[5] = static_cast<uint8_t>((event_idx >> 8) & 0xffU);
  fence->payload[6] = static_cast<uint8_t>((event_idx >> 16) & 0xffU);
  fence->payload[7] = static_cast<uint8_t>((event_idx >> 24) & 0xffU);
  for (int i = 0; i < 8; ++i) {
    fence->payload[8 + i] =
        static_cast<uint8_t>((engine_id >> (i * 8)) & 0xffU);
  }
}

bool LoadFenceOwner(const NvSciSyncFence& fence, uint64_t* engine_id,
                    uint32_t* event_idx = nullptr) {
  if (!GetNvSciFenceEngineId(fence, engine_id)) {
    return false;
  }
  if (event_idx != nullptr) {
    *event_idx = static_cast<uint32_t>(fence.payload[4]) |
                 (static_cast<uint32_t>(fence.payload[5]) << 8) |
                 (static_cast<uint32_t>(fence.payload[6]) << 16) |
                 (static_cast<uint32_t>(fence.payload[7]) << 24);
  }
  return true;
}

bool IsHostSynchronizedFence(const NvSciSyncFence& fence, uint64_t* owner) {
  return fence.payload[16] == kHostSynchronizedFence &&
         LoadFenceOwner(fence, owner) &&
         static_cast<uint32_t>(fence.fence_id >> 32) ==
             static_cast<uint32_t>(*owner);
}

}  // namespace

NvSciSyncEngine::NvSciSyncEngine() : NvSciSyncEngine(1) {}

NvSciSyncEngine::NvSciSyncEngine(uint64_t engine_id, bool host_synchronized)
    : engine_id_(engine_id), host_synchronized_(host_synchronized) {}

NvSciSyncEngine::~NvSciSyncEngine() {
#if defined(CYBER_USE_CUDA_IPC)
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& pair : imported_cuda_events_) {
    for (auto* ev : pair.second) {
      if (ev != nullptr) {
        cudaEventDestroy(ev);
      }
    }
  }
  imported_cuda_events_.clear();
  for (size_t i = 0; i < kEventRingSize; ++i) {
    if (cuda_events_[i] != nullptr) {
      cudaEventDestroy(cuda_events_[i]);
      cuda_events_[i] = nullptr;
    }
  }
#endif
}

#if defined(CYBER_USE_CUDA_IPC)
bool NvSciSyncEngine::EnsureCudaIpcEvents() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cuda_events_[0] != nullptr) {
    return true;
  }
  for (size_t i = 0; i < kEventRingSize; ++i) {
    cudaError_t err = cudaEventCreateWithFlags(
        &cuda_events_[i], cudaEventDisableTiming | cudaEventInterprocess);
    if (err != cudaSuccess || cuda_events_[i] == nullptr) {
      for (size_t j = 0; j < i; ++j) {
        if (cuda_events_[j] != nullptr) {
          cudaEventDestroy(cuda_events_[j]);
          cuda_events_[j] = nullptr;
        }
      }
      return false;
    }
  }
  return true;
}
#endif

NvSciSyncFence NvSciSyncEngine::GenerateSignalFence(void* stream_ptr) {
  NvSciSyncFence fence;
  const uint64_t fid = fence_seq_.fetch_add(1, std::memory_order_relaxed);
  fence.fence_id = (engine_id_ << 32) | (fid & 0xFFFFFFFF);

  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  fence.timestamp_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

  uint32_t event_idx =
      static_cast<uint32_t>(fid % NvSciSyncEngine::kEventRingSize);

#if defined(CYBER_USE_CUDA_IPC)
  if (host_synchronized_) {
    if (stream_ptr != nullptr &&
        cudaStreamSynchronize(static_cast<cudaStream_t>(stream_ptr)) !=
            cudaSuccess) {
      fence.Reset();
      return fence;
    }
    StoreFenceOwner(engine_id_, event_idx, &fence);
    fence.payload[16] = kHostSynchronizedFence;
    return fence;
  }
  if (stream_ptr != nullptr) {
    cudaStream_t stream = static_cast<cudaStream_t>(stream_ptr);
    if (!EnsureCudaIpcEvents()) {
      fence.Reset();
      return fence;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    bool event_available = false;
    for (size_t offset = 0; offset < kEventRingSize; ++offset) {
      const uint32_t candidate = static_cast<uint32_t>(
          (event_idx + offset) % NvSciSyncEngine::kEventRingSize);
      if (!event_recorded_[candidate] ||
          cudaEventQuery(cuda_events_[candidate]) == cudaSuccess) {
        event_idx = candidate;
        event_available = true;
        break;
      }
    }
    if (!event_available ||
        cudaEventRecord(cuda_events_[event_idx], stream) != cudaSuccess) {
      fence.Reset();
      return fence;
    }
    event_recorded_[event_idx] = true;
    StoreFenceOwner(engine_id_, event_idx, &fence);
  } else {
    std::lock_guard<std::mutex> lock(mutex_);
    StoreFenceOwner(engine_id_, event_idx, &fence);
    pending_streamless_fences_.insert(fence.fence_id);
  }
#elif defined(CYBER_USE_NVSCI)
  StoreFenceOwner(engine_id_, event_idx, &fence);
  orin::OrinNvSciBackend::GenerateNvSciFence(stream_ptr, &fence);
  if (stream_ptr == nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_streamless_fences_.insert(fence.fence_id);
  }
#else
  (void)stream_ptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    StoreFenceOwner(engine_id_, event_idx, &fence);
    pending_streamless_fences_.insert(fence.fence_id);
  }
#endif

  return fence;
}

bool NvSciSyncEngine::InsertWaitFence(void* stream_ptr,
                                      const NvSciSyncFence& fence) {
  if (!fence.IsValid()) {
    return false;
  }
#if defined(CYBER_USE_CUDA_IPC)
  if (host_synchronized_) {
    uint64_t owner = 0;
    if (!IsHostSynchronizedFence(fence, &owner)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return owner == engine_id_ ||
           imported_host_engines_.find(owner) != imported_host_engines_.end();
  }
  cudaStream_t stream = static_cast<cudaStream_t>(stream_ptr);
  if (stream != nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t owner = 0;
    uint32_t event_idx = 0;
    if (!LoadFenceOwner(fence, &owner, &event_idx)) {
      return false;
    }
    cudaEvent_t event = nullptr;
    if (owner == engine_id_) {
      if (cuda_events_[0] != nullptr) {
        event = cuda_events_[event_idx % kEventRingSize];
      }
    } else {
      const auto it = imported_cuda_events_.find(owner);
      if (it != imported_cuda_events_.end() && !it->second.empty()) {
        event = it->second[event_idx % it->second.size()];
      }
    }
    if (event != nullptr) {
      return cudaStreamWaitEvent(stream, event, 0) == cudaSuccess;
    }
    return completed_fences_.find(fence.fence_id) != completed_fences_.end();
  }
  return true;
#elif defined(CYBER_USE_NVSCI)
  return orin::OrinNvSciBackend::InsertWaitNvSciFence(stream_ptr, fence);
#else
  (void)stream_ptr;
  return true;
#endif
}

bool NvSciSyncEngine::ExportSyncObj(std::vector<uint8_t>* sync_desc) {
  if (!sync_desc) {
    return false;
  }
#if defined(CYBER_USE_CUDA_IPC)
  if (host_synchronized_) {
    sync_desc->clear();
    sync_desc->reserve(32);
    wire::AppendU32(kSyncDescriptorMagic, sync_desc);
    wire::AppendU32(kSyncDescriptorVersion, sync_desc);
    wire::AppendU32(kSyncBackendHostSynchronized, sync_desc);
    wire::AppendU32(0, sync_desc);
    wire::AppendU64(engine_id_, sync_desc);
    wire::AppendU32(0, sync_desc);
    wire::AppendU32(0, sync_desc);
    return true;
  }
  if (!EnsureCudaIpcEvents()) {
    sync_desc->clear();
    return false;
  }
  std::array<cudaIpcEventHandle_t, kEventRingSize> handles{};
  for (size_t i = 0; i < kEventRingSize; ++i) {
    if (cudaIpcGetEventHandle(&handles[i], cuda_events_[i]) != cudaSuccess) {
      sync_desc->clear();
      return false;
    }
  }
  sync_desc->clear();
  sync_desc->reserve(32 + sizeof(handles));
  wire::AppendU32(kSyncDescriptorMagic, sync_desc);
  wire::AppendU32(kSyncDescriptorVersion, sync_desc);
  wire::AppendU32(kSyncBackendCudaIpc, sync_desc);
  wire::AppendU32(static_cast<uint32_t>(kEventRingSize), sync_desc);
  wire::AppendU64(engine_id_, sync_desc);
  wire::AppendU32(static_cast<uint32_t>(sizeof(handles)), sync_desc);
  wire::AppendU32(0, sync_desc);
  const auto* bytes = reinterpret_cast<const uint8_t*>(handles.data());
  sync_desc->insert(sync_desc->end(), bytes, bytes + sizeof(handles));
  return true;
#else
  sync_desc->clear();
  return false;
#endif
}

bool NvSciSyncEngine::ImportSyncObj(const std::vector<uint8_t>& sync_desc) {
#if defined(CYBER_USE_CUDA_IPC)
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t backend = 0;
  uint32_t event_count = 0;
  uint64_t peer_engine = 0;
  uint32_t handle_size = 0;
  uint32_t reserved = 0;
  if (!wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset, &magic) ||
      !wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset, &version) ||
      !wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset, &backend) ||
      !wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset,
                     &event_count) ||
      !wire::ReadU64(sync_desc.data(), sync_desc.size(), &offset,
                     &peer_engine) ||
      !wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset,
                     &handle_size) ||
      !wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset, &reserved) ||
      magic != kSyncDescriptorMagic || version != kSyncDescriptorVersion ||
      peer_engine == 0) {
    return false;
  }
  if (backend == kSyncBackendHostSynchronized) {
    if (!host_synchronized_ || event_count != 0 || handle_size != 0 ||
        sync_desc.size() != offset) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    imported_host_engines_.insert(peer_engine);
    return true;
  }
  if (backend != kSyncBackendCudaIpc || host_synchronized_) {
    return false;
  }
  if (peer_engine == engine_id_) {
    return EnsureCudaIpcEvents();
  }
  const size_t single_handle_size = sizeof(cudaIpcEventHandle_t);
  const size_t total_handles = handle_size / single_handle_size;
  if (total_handles == 0 || (handle_size % single_handle_size) != 0 ||
      sync_desc.size() - offset != handle_size) {
    return false;
  }

  std::vector<cudaEvent_t> opened_events;
  opened_events.reserve(total_handles);
  for (size_t i = 0; i < total_handles; ++i) {
    cudaIpcEventHandle_t handle{};
    std::memcpy(&handle, sync_desc.data() + offset + i * single_handle_size,
                single_handle_size);
    cudaEvent_t imported = nullptr;
    if (cudaIpcOpenEventHandle(&imported, handle) != cudaSuccess ||
        imported == nullptr) {
      for (auto* ev : opened_events) {
        cudaEventDestroy(ev);
      }
      return false;
    }
    opened_events.push_back(imported);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  imported_host_engines_.erase(peer_engine);
  auto it = imported_cuda_events_.find(peer_engine);
  if (it != imported_cuda_events_.end()) {
    for (auto* ev : opened_events) {
      cudaEventDestroy(ev);
    }
    return true;
  }
  imported_cuda_events_[peer_engine] = std::move(opened_events);
  return true;
#else
  (void)sync_desc;
  return false;
#endif
}

void NvSciSyncEngine::ReleaseSyncObj(const std::vector<uint8_t>& sync_desc) {
#if defined(CYBER_USE_CUDA_IPC)
  uint64_t peer_engine = 0;
  if (!GetSyncObjId(sync_desc, &peer_engine)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  imported_host_engines_.erase(peer_engine);
  auto it = imported_cuda_events_.find(peer_engine);
  if (it != imported_cuda_events_.end()) {
    for (auto* ev : it->second) {
      if (ev != nullptr) {
        cudaEventDestroy(ev);
      }
    }
    imported_cuda_events_.erase(it);
  }
#else
  (void)sync_desc;
#endif
}

bool NvSciSyncEngine::GetSyncObjId(const std::vector<uint8_t>& sync_desc,
                                   uint64_t* engine_id) const {
  if (engine_id == nullptr) {
    return false;
  }
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t backend = 0;
  uint32_t reserved = 0;
  return wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset, &magic) &&
         wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset, &version) &&
         wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset, &backend) &&
         wire::ReadU32(sync_desc.data(), sync_desc.size(), &offset,
                       &reserved) &&
         wire::ReadU64(sync_desc.data(), sync_desc.size(), &offset,
                       engine_id) &&
         magic == kSyncDescriptorMagic && version == kSyncDescriptorVersion &&
         (backend == kSyncBackendCudaIpc ||
          backend == kSyncBackendHostSynchronized) &&
         *engine_id != 0;
}

bool NvSciSyncEngine::IsFenceSignaled(const NvSciSyncFence& fence) {
  if (!fence.IsValid()) {
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (completed_fences_.find(fence.fence_id) != completed_fences_.end()) {
    return true;
  }
  if (pending_streamless_fences_.find(fence.fence_id) !=
      pending_streamless_fences_.end()) {
    return false;
  }
#if defined(CYBER_USE_CUDA_IPC)
  if (host_synchronized_) {
    uint64_t owner = 0;
    if (!IsHostSynchronizedFence(fence, &owner)) {
      return false;
    }
    return owner == engine_id_ ||
           imported_host_engines_.find(owner) != imported_host_engines_.end();
  }
  uint64_t owner = 0;
  uint32_t event_idx = 0;
  if (!LoadFenceOwner(fence, &owner, &event_idx)) {
    return false;
  }
  cudaEvent_t event = nullptr;
  if (owner == engine_id_) {
    if (cuda_events_[0] != nullptr) {
      event = cuda_events_[event_idx % kEventRingSize];
    }
  } else {
    const auto it = imported_cuda_events_.find(owner);
    if (it != imported_cuda_events_.end() && !it->second.empty()) {
      event = it->second[event_idx % it->second.size()];
    }
  }
  if (event != nullptr) {
    return cudaEventQuery(event) == cudaSuccess;
  }
  return false;
#elif defined(CYBER_USE_NVSCI)
  return true;
#endif
  return true;
}

void NvSciSyncEngine::MarkFenceCompleted(uint64_t fence_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  completed_fences_.insert(fence_id);
  pending_streamless_fences_.erase(fence_id);
}

void NvSciSyncEngine::PruneSignaledEvents() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (completed_fences_.size() > 1024) {
    completed_fences_.clear();
  }
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

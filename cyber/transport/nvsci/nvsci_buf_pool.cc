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
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <string>

#if defined(CYBER_USE_CUDA_IPC)
#include <cuda_runtime.h>
#endif

#include "cyber/transport/nvsci/gpu_wire.h"
#include "cyber/transport/nvsci/orin_nvsci_backend.h"

namespace apollo {
namespace cyber {
namespace transport {

namespace {

static std::atomic<uint64_t> g_pool_instance_counter{1};

static int CreateUniqueSharedMemory(uint64_t pool_uid, uint32_t slot_id,
                                    std::string* shm_name) {
  if (shm_name == nullptr) {
    return -1;
  }
  std::random_device random_device;
  for (int attempt = 0; attempt < 128; ++attempt) {
    const uint64_t nonce =
        (static_cast<uint64_t>(random_device()) << 32) ^ random_device();
    *shm_name = "/cyber_gpu_" + std::to_string(pool_uid) + "_" +
                std::to_string(slot_id) + "_" + std::to_string(nonce);
    const int fd = shm_open(shm_name->c_str(),
                            O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0) {
      return fd;
    }
    if (errno != EEXIST) {
      break;
    }
  }
  shm_name->clear();
  return -1;
}

static bool IsIntegratedGpu() {
#if defined(CYBER_USE_CUDA_IPC)
  static const bool is_integrated = []() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
      return false;
    }
    int dev = 0;
    if (cudaGetDevice(&dev) != cudaSuccess) {
      return false;
    }
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, dev) == cudaSuccess) {
      return prop.integrated == 1;
    }
    return false;
  }();
  return is_integrated;
#else
  return false;
#endif
}

}  // namespace

NvSciBufPool::NvSciBufPool(const NvSciBufPoolConfig& config) : config_(config) {
  if (config_.slot_count == 0) {
    config_.slot_count = 4;
  }
  if (config_.slot_size == 0) {
    config_.slot_size = 4 * 1024 * 1024;
  }
  pool_uid_ = (static_cast<uint64_t>(getpid()) << 32) |
              g_pool_instance_counter.fetch_add(1);
}

NvSciBufPool::~NvSciBufPool() {
  for (auto& entry : slots_) {
    if (entry) {
      if (entry->host_shm_ptr != nullptr) {
#if defined(CYBER_USE_CUDA_IPC)
        cudaHostUnregister(entry->host_shm_ptr);
#endif
        munmap(entry->host_shm_ptr, config_.slot_size);
        if (entry->shm_fd >= 0) {
          close(entry->shm_fd);
          entry->shm_fd = -1;
        }
        if (entry->is_shm_creator && !entry->shm_name.empty()) {
          shm_unlink(entry->shm_name.c_str());
        }
        entry->host_shm_ptr = nullptr;
        entry->dev_ptr = nullptr;
      }
#if defined(CYBER_USE_CUDA_IPC)
      if (entry->is_ipc_opened && entry->dev_ptr) {
        cudaIpcCloseMemHandle(entry->dev_ptr);
        entry->dev_ptr = nullptr;
      } else if (entry->is_cuda_allocated && entry->dev_ptr) {
        cudaFree(entry->dev_ptr);
        entry->dev_ptr = nullptr;
      }
#elif defined(CYBER_USE_NVSCI)
      if (entry->dev_ptr) {
        orin::OrinNvSciBackend::FreeNvSciBuffer(entry->dev_ptr);
        entry->dev_ptr = nullptr;
      }
#endif
      entry->dev_ptr = nullptr;
    }
  }
}

bool NvSciBufPool::Initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_initialized_) {
    return true;
  }

  const bool is_integrated = IsIntegratedGpu();

  slots_.reserve(config_.slot_count);
  for (uint32_t i = 0; i < config_.slot_count; ++i) {
    auto entry = std::make_unique<SlotEntry>();
    entry->slot_id = static_cast<int>(i);
    entry->state.store(SlotState::FREE, std::memory_order_relaxed);
    entry->ref_count.store(0, std::memory_order_relaxed);
    entry->last_post_fences.clear();

    bool allocated = false;
#if defined(CYBER_USE_CUDA_IPC)
    if (is_integrated) {
  std::string shm_name;
  int fd = CreateUniqueSharedMemory(pool_uid_, i, &shm_name);
      if (fd >= 0) {
        if (ftruncate(fd, config_.slot_size) == 0) {
          void* h_ptr = mmap(NULL, config_.slot_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
          if (h_ptr != MAP_FAILED) {
            cudaError_t reg_err =
                cudaHostRegister(h_ptr, config_.slot_size, cudaHostRegisterMapped);
            if (reg_err == cudaSuccess) {
              void* d_ptr = nullptr;
              cudaError_t ptr_err = cudaHostGetDevicePointer(&d_ptr, h_ptr, 0);
              if (ptr_err == cudaSuccess && d_ptr != nullptr) {
                entry->dev_ptr = d_ptr;
                entry->host_shm_ptr = h_ptr;
                entry->shm_name = shm_name;
                entry->shm_fd = fd;
                entry->is_shm_creator = true;
                entry->is_cuda_allocated = false;
                allocated = true;
              } else {
                cudaHostUnregister(h_ptr);
              }
            }
            if (!allocated) {
              munmap(h_ptr, config_.slot_size);
            }
          }
        }
        if (!allocated) {
          close(fd);
          shm_unlink(shm_name.c_str());
        }
      }
    }

    if (!allocated) {
      void* d_ptr = nullptr;
      cudaError_t err = cudaMalloc(&d_ptr, config_.slot_size);
      if (err == cudaSuccess && d_ptr != nullptr) {
        entry->dev_ptr = d_ptr;
        entry->is_cuda_allocated = true;
        allocated = true;
      }
    }
#endif

    if (!allocated) {
      AWARN << "CUDA allocation unavailable, falling back to aligned host memory";
      const size_t total_size = config_.slot_size + config_.alignment;
      entry->allocated_storage.resize(total_size, 0);
      uintptr_t raw_addr =
          reinterpret_cast<uintptr_t>(entry->allocated_storage.data());
      uintptr_t aligned_addr = (raw_addr + config_.alignment - 1) &
                               ~(static_cast<uintptr_t>(config_.alignment - 1));
      entry->dev_ptr = reinterpret_cast<void*>(aligned_addr);
      entry->is_cuda_allocated = false;
    }

    slots_.push_back(std::move(entry));
  }

  is_initialized_ = true;
  return true;
}

int NvSciBufPool::AcquireSlot() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_initialized_ || slots_.empty()) {
    return -1;
  }

  const uint32_t total = config_.slot_count;
  for (uint32_t attempt = 0; attempt < total; ++attempt) {
    uint32_t idx = (next_slot_hint_ + attempt) % total;
    auto& entry = slots_[idx];
    SlotState expected = SlotState::FREE;
    if (entry->state.compare_exchange_strong(expected, SlotState::LOANED,
                                             std::memory_order_acq_rel)) {
      entry->ref_count.store(1, std::memory_order_relaxed);
      next_slot_hint_ = (idx + 1) % total;
      return entry->slot_id;
    }
  }

  // All slots currently busy
  return -1;
}

bool NvSciBufPool::MarkInUse(int slot_id, int32_t ref_count) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size()) ||
      ref_count <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto& entry = slots_[slot_id];
  if (entry->state.load(std::memory_order_relaxed) != SlotState::LOANED) {
    return false;
  }
  entry->last_post_fences.clear();
  entry->ref_count.store(ref_count, std::memory_order_relaxed);
  entry->state.store(SlotState::IN_USE, std::memory_order_release);
  return true;
}

bool NvSciBufPool::ReleaseSlot(int slot_id, const NvSciSyncFence& post_fence) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto& entry = slots_[slot_id];
  const SlotState state = entry->state.load(std::memory_order_relaxed);
  if (state != SlotState::LOANED && state != SlotState::IN_USE) {
    return false;
  }
  int32_t cur_ref = entry->ref_count.load(std::memory_order_relaxed);
  if (cur_ref <= 0) {
    return false;  // Prevent underflow
  }

  if (post_fence.IsValid()) {
    entry->last_post_fences.push_back(post_fence);
  }

  int32_t prev_ref = entry->ref_count.fetch_sub(1, std::memory_order_acq_rel);
  if (prev_ref <= 1) {
    entry->state.store(SlotState::FREE, std::memory_order_release);
    return true;
  }
  return true;
}

void* NvSciBufPool::GetDevicePtr(int slot_id) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return nullptr;
  }
  return slots_[slot_id]->dev_ptr;
}

SlotState NvSciBufPool::GetSlotState(int slot_id) const {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return SlotState::FREE;
  }
  return slots_[slot_id]->state.load(std::memory_order_acquire);
}

bool NvSciBufPool::ExportBuffer(int slot_id, std::vector<uint8_t>* ipc_desc) {
  if (!ipc_desc || slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return false;
  }
#if defined(CYBER_USE_CUDA_IPC)
  auto& entry = slots_[slot_id];
  // 1. Orin UMA zero-copy POSIX shared memory export
  if (entry->host_shm_ptr != nullptr && !entry->shm_name.empty()) {
    ipc_desc->clear();
    const uint32_t name_len = static_cast<uint32_t>(entry->shm_name.size());
    ipc_desc->reserve(kGpuBufferDescriptorHeaderSize + name_len);
    wire::AppendU32(kOrinUmaBufferMagic, ipc_desc);
    wire::AppendU32(kGpuBufferDescriptorVersion, ipc_desc);
    wire::AppendU32(static_cast<uint32_t>(slot_id), ipc_desc);
    wire::AppendU32(name_len, ipc_desc);
    wire::AppendU64(config_.slot_size, ipc_desc);
    ipc_desc->insert(ipc_desc->end(), entry->shm_name.begin(),
                     entry->shm_name.end());
    return true;
  }

  // 2. Standard CUDA IPC handle export (for dGPU)
  if (entry->is_cuda_allocated && entry->dev_ptr) {
    cudaIpcMemHandle_t handle{};
    cudaError_t err = cudaIpcGetMemHandle(&handle, entry->dev_ptr);
    if (err == cudaSuccess) {
      ipc_desc->clear();
      ipc_desc->reserve(kGpuBufferDescriptorHeaderSize + sizeof(handle));
      wire::AppendU32(kCudaIpcBufferMagic, ipc_desc);
      wire::AppendU32(kGpuBufferDescriptorVersion, ipc_desc);
      wire::AppendU32(static_cast<uint32_t>(slot_id), ipc_desc);
      wire::AppendU32(static_cast<uint32_t>(sizeof(handle)), ipc_desc);
      wire::AppendU64(config_.slot_size, ipc_desc);
      const auto* bytes = reinterpret_cast<const uint8_t*>(&handle);
      ipc_desc->insert(ipc_desc->end(), bytes, bytes + sizeof(handle));
      return true;
    } else {
      AERROR << "cudaIpcGetMemHandle failed for slot " << slot_id << ": "
             << cudaGetErrorString(err);
    }
  }
#elif defined(CYBER_USE_NVSCI)
  if (orin::OrinNvSciBackend::AllocateNvSciBuffer(
          config_.slot_size, config_.alignment, &slots_[slot_id]->dev_ptr,
          ipc_desc)) {
    return true;
  }
#endif

  ipc_desc->clear();
  ipc_desc->reserve(kGpuBufferDescriptorHeaderSize);
  wire::AppendU32(kSimulatedBufferMagic, ipc_desc);
  wire::AppendU32(kGpuBufferDescriptorVersion, ipc_desc);
  wire::AppendU32(static_cast<uint32_t>(slot_id), ipc_desc);
  wire::AppendU32(0, ipc_desc);
  wire::AppendU64(config_.slot_size, ipc_desc);
  return true;
}

bool NvSciBufPool::ImportBuffer(int slot_id,
                                const std::vector<uint8_t>& ipc_desc) {
  return ImportBufferInternal(slot_id, ipc_desc, false);
}

bool NvSciBufPool::ImportBufferStrict(int slot_id,
                                      const std::vector<uint8_t>& ipc_desc) {
  return ImportBufferInternal(slot_id, ipc_desc, true);
}

bool NvSciBufPool::ImportBufferInternal(int slot_id,
                                        const std::vector<uint8_t>& ipc_desc,
                                        bool strict) {
  if (ipc_desc.size() < kGpuBufferDescriptorHeaderSize || slot_id < 0 ||
      slot_id >= static_cast<int>(slots_.size())) {
    return false;
  }
  size_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t sid = 0;
  uint32_t handle_size = 0;
  uint64_t cap = 0;
  if (!wire::ReadU32(ipc_desc.data(), ipc_desc.size(), &offset, &magic) ||
      !wire::ReadU32(ipc_desc.data(), ipc_desc.size(), &offset, &version) ||
      !wire::ReadU32(ipc_desc.data(), ipc_desc.size(), &offset, &sid) ||
      !wire::ReadU32(ipc_desc.data(), ipc_desc.size(), &offset, &handle_size) ||
      !wire::ReadU64(ipc_desc.data(), ipc_desc.size(), &offset, &cap) ||
      version != kGpuBufferDescriptorVersion ||
      sid != static_cast<uint32_t>(slot_id) || cap != config_.slot_size) {
    return false;
  }

#if defined(CYBER_USE_CUDA_IPC)
  if (magic == kOrinUmaBufferMagic && handle_size > 0 &&
      ipc_desc.size() - offset == handle_size) {
    std::string peer_shm_name(
        reinterpret_cast<const char*>(ipc_desc.data() + offset), handle_size);
    if (peer_shm_name.front() != '/' ||
        peer_shm_name.find('\0') != std::string::npos ||
        peer_shm_name.find('/', 1) != std::string::npos) {
      return false;
    }
    int fd = shm_open(peer_shm_name.c_str(), O_RDWR | O_CLOEXEC, 0);
    if (fd >= 0) {
      struct stat shm_stat {};
      void* h_ptr = MAP_FAILED;
      if (fstat(fd, &shm_stat) == 0 && shm_stat.st_size >= 0 &&
          static_cast<uint64_t>(shm_stat.st_size) == config_.slot_size) {
        h_ptr = mmap(NULL, config_.slot_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
      }
      if (h_ptr != MAP_FAILED) {
        cudaError_t reg_err =
            cudaHostRegister(h_ptr, config_.slot_size, cudaHostRegisterMapped);
        if (reg_err == cudaSuccess) {
          void* d_ptr = nullptr;
          cudaError_t ptr_err = cudaHostGetDevicePointer(&d_ptr, h_ptr, 0);
          if (ptr_err == cudaSuccess && d_ptr != nullptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& entry = slots_[slot_id];
            // Clean up old resources if any
            if (entry->host_shm_ptr != nullptr) {
              cudaHostUnregister(entry->host_shm_ptr);
              munmap(entry->host_shm_ptr, config_.slot_size);
              if (entry->shm_fd >= 0) close(entry->shm_fd);
              if (entry->is_shm_creator && !entry->shm_name.empty()) {
                shm_unlink(entry->shm_name.c_str());
              }
            } else if (entry->is_ipc_opened && entry->dev_ptr) {
              cudaIpcCloseMemHandle(entry->dev_ptr);
            } else if (entry->is_cuda_allocated && entry->dev_ptr) {
              cudaFree(entry->dev_ptr);
            }
            entry->dev_ptr = d_ptr;
            entry->host_shm_ptr = h_ptr;
            entry->shm_name = peer_shm_name;
            entry->shm_fd = fd;
            entry->is_shm_creator = false;
            entry->is_ipc_opened = false;
            entry->is_cuda_allocated = false;
            return true;
          } else {
            cudaHostUnregister(h_ptr);
          }
        }
        munmap(h_ptr, config_.slot_size);
      }
      close(fd);
    }
    AWARN << "Failed to import Orin UMA buffer for slot " << slot_id << " from "
          << peer_shm_name;
    return strict ? false : true;
  }

  if (magic == kCudaIpcBufferMagic &&
      handle_size == sizeof(cudaIpcMemHandle_t) &&
      ipc_desc.size() - offset == sizeof(cudaIpcMemHandle_t)) {
    cudaIpcMemHandle_t handle{};
    std::memcpy(&handle, ipc_desc.data() + offset, sizeof(handle));
    void* imported_ptr = nullptr;
    cudaError_t err = cudaIpcOpenMemHandle(&imported_ptr, handle,
                                           cudaIpcMemLazyEnablePeerAccess);
    if (err == cudaSuccess && imported_ptr != nullptr) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto& entry = slots_[slot_id];
      if (entry->is_ipc_opened && entry->dev_ptr) {
        cudaIpcCloseMemHandle(entry->dev_ptr);
      } else if (entry->is_cuda_allocated && entry->dev_ptr) {
        cudaFree(entry->dev_ptr);
      }
      entry->dev_ptr = imported_ptr;
      entry->is_ipc_opened = true;
      entry->is_cuda_allocated = false;
      return true;
    } else {
      AWARN << "cudaIpcOpenMemHandle failed (peer process access): "
            << cudaGetErrorString(err) << ", keeping local mapping";
      return strict ? false : true;
    }
  }
#elif defined(CYBER_USE_NVSCI)
  void* imported_ptr = nullptr;
  if (orin::OrinNvSciBackend::ImportNvSciBuffer(ipc_desc, &imported_ptr)) {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_[slot_id]->dev_ptr = imported_ptr;
    return true;
  }
#endif

  return !strict && magic == kSimulatedBufferMagic && handle_size == 0 &&
         ipc_desc.size() == offset;
}

NvSciSyncFence NvSciBufPool::GetLastPostFence(int slot_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size()) ||
      slots_[slot_id]->last_post_fences.empty()) {
    return NvSciSyncFence{};
  }
  return slots_[slot_id]->last_post_fences.back();
}

std::vector<NvSciSyncFence> NvSciBufPool::GetLastPostFences(int slot_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return {};
  }
  return slots_[slot_id]->last_post_fences;
}

void NvSciBufPool::AddPostFence(int slot_id, const NvSciSyncFence& post_fence) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size()) ||
      !post_fence.IsValid()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  slots_[slot_id]->last_post_fences.push_back(post_fence);
}

void NvSciBufPool::ClearPostFences(int slot_id) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  slots_[slot_id]->last_post_fences.clear();
}

void NvSciBufPool::ClearPostFencesForEngine(uint64_t engine_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& slot : slots_) {
    auto& fences = slot->last_post_fences;
    fences.erase(std::remove_if(fences.begin(), fences.end(),
                                [engine_id](const NvSciSyncFence& fence) {
                                  uint64_t owner = 0;
                                  return GetNvSciFenceEngineId(fence, &owner) &&
                                         owner == engine_id;
                                }),
                 fences.end());
  }
}

bool NvSciBufPool::QuarantineSlot(int slot_id) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (slots_[slot_id]->state.load(std::memory_order_relaxed) !=
      SlotState::IN_USE) {
    return false;
  }
  slots_[slot_id]->state.store(SlotState::QUARANTINED,
                               std::memory_order_release);
  return true;
}

bool NvSciBufPool::UnquarantineSlot(int slot_id) {
  if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto& entry = slots_[slot_id];
  if (entry->state.load(std::memory_order_relaxed) == SlotState::QUARANTINED) {
    entry->ref_count.store(0, std::memory_order_relaxed);
    entry->state.store(SlotState::FREE, std::memory_order_release);
    return true;
  }
  return false;
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

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

#include "cyber/transport/nvsci/gpu_channel_manager.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <utility>

#include "cyber/common/util.h"
#include "cyber/transport/nvsci/gpu_wire.h"

namespace apollo {
namespace cyber {
namespace transport {
namespace {

std::atomic<uint64_t> g_session_sequence{1};

uint64_t NextSessionId() {
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::system_clock::now().time_since_epoch().count());
  const uint64_t sequence =
      g_session_sequence.fetch_add(1, std::memory_order_relaxed);
  uint64_t id = now ^ (static_cast<uint64_t>(getpid()) << 32) ^ sequence;
  return id == 0 ? sequence : id;
}

GpuBufferBackend DetectBackend(const GpuBufferDescriptor& descriptor) {
  if (descriptor.backend != GpuBufferBackend::UNKNOWN) {
    return descriptor.backend;
  }
  if (descriptor.nvsci_buf_ipc_desc.size() >= kGpuBufferDescriptorHeaderSize) {
    size_t offset = 0;
    uint32_t magic = 0;
    if (wire::ReadU32(descriptor.nvsci_buf_ipc_desc.data(),
                      descriptor.nvsci_buf_ipc_desc.size(), &offset, &magic) &&
        magic == kCudaIpcBufferMagic) {
      return GpuBufferBackend::CUDA_IPC;
    }
  }
  return GpuBufferBackend::UNKNOWN;
}

bool IsSupportedDescriptor(const GpuBufferDescriptor& descriptor) {
  const auto backend = DetectBackend(descriptor);
  if (backend == GpuBufferBackend::CUDA_IPC) {
    return true;
  }
#if defined(CYBER_USE_NVSCI)
  return backend == GpuBufferBackend::NVSCI_BUF;
#else
  return false;
#endif
}

}  // namespace

GpuChannelManager* GpuChannelManager::Instance() {
  static GpuChannelManager instance;
  return &instance;
}

GpuChannelSessionPtr GpuChannelManager::GetOrCreateSession(
    const std::string& channel_name, const NvSciBufPoolConfig& config,
    uint64_t sync_engine_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(channel_name);
  if (it != sessions_.end()) {
    return it->second;
  }

  uint64_t channel_id = static_cast<uint64_t>(common::Hash(channel_name));
  auto pool = std::make_shared<NvSciBufPool>(config);
  if (!pool->Initialize()) {
    return nullptr;
  }

  uint64_t engine_id = sync_engine_id != 0 ? sync_engine_id : channel_id;
  auto sync_engine = std::make_shared<NvSciSyncEngine>(engine_id);
  auto session = std::make_shared<GpuChannelSession>(
      channel_id, pool, sync_engine, NextSessionId());

  sessions_[channel_name] = session;
  return session;
}

GpuChannelIpcStatus GpuChannelManager::ExportSession(
    const std::string& channel_name, GpuSessionDescriptor* descriptor) const {
  if (descriptor == nullptr) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }
  GpuChannelSessionPtr session = GetSession(channel_name);
  if (!session || !session->pool() || !session->sync_engine()) {
    return GpuChannelIpcStatus::kSessionNotFound;
  }
  GpuSessionDescriptor exported;
  exported.channel_id = session->channel_id();
  exported.session_id = session->session_id();
  exported.config.slot_count =
      static_cast<uint32_t>(session->pool()->GetSlotCount());
  exported.config.slot_size = session->pool()->GetSlotCapacity();
  exported.config.alignment = session->pool()->GetAlignment();
  if (!session->sync_engine()->ExportSyncObj(&exported.producer_sync_desc)) {
    return GpuChannelIpcStatus::kUnsupported;
  }
  const auto status = ExportSession(channel_name, &exported.buffers);
  if (status != GpuChannelIpcStatus::kSuccess) {
    return status;
  }
  exported.backend = exported.buffers.front().backend;
  *descriptor = std::move(exported);
  return GpuChannelIpcStatus::kSuccess;
}

GpuChannelSessionPtr GpuChannelManager::GetSession(
    const std::string& channel_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(channel_name);
  if (it != sessions_.end()) {
    return it->second;
  }
  return nullptr;
}

GpuChannelIpcStatus GpuChannelManager::ExportSession(
    const std::string& channel_name,
    std::vector<GpuBufferDescriptor>* descriptors) const {
  if (channel_name.empty() || descriptors == nullptr) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }

  GpuChannelSessionPtr session = GetSession(channel_name);
  if (!session || !session->pool()) {
    return GpuChannelIpcStatus::kSessionNotFound;
  }

  std::vector<GpuBufferDescriptor> exported;
  const auto pool = session->pool();
  exported.reserve(pool->GetSlotCount());
  for (size_t i = 0; i < pool->GetSlotCount(); ++i) {
    GpuBufferDescriptor descriptor;
    descriptor.slot_id = static_cast<uint32_t>(i);
    descriptor.capacity = pool->GetSlotCapacity();
    if (!pool->ExportBuffer(static_cast<int>(i),
                            &descriptor.nvsci_buf_ipc_desc)) {
      descriptors->clear();
      return GpuChannelIpcStatus::kExportFailed;
    }
    descriptor.backend = DetectBackend(descriptor);
    if (!IsSupportedDescriptor(descriptor)) {
      descriptors->clear();
      return GpuChannelIpcStatus::kUnsupported;
    }
    exported.push_back(std::move(descriptor));
  }

  *descriptors = std::move(exported);
  return GpuChannelIpcStatus::kSuccess;
}

GpuChannelIpcStatus GpuChannelManager::ImportSession(
    const std::string& channel_name, const NvSciBufPoolConfig& config,
    const std::vector<GpuBufferDescriptor>& descriptors,
    uint64_t sync_engine_id,
    const std::vector<uint8_t>& producer_sync_desc) {
  if (channel_name.empty() || descriptors.empty() ||
      descriptors.size() != config.slot_count) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (sessions_.find(channel_name) != sessions_.end()) {
    return GpuChannelIpcStatus::kSessionAlreadyExists;
  }

  auto pool = std::make_shared<NvSciBufPool>(config);
  if (!pool->Initialize()) {
    return GpuChannelIpcStatus::kImportFailed;
  }

  for (size_t i = 0; i < descriptors.size(); ++i) {
    const auto& descriptor = descriptors[i];
    if (descriptor.slot_id != i || descriptor.capacity != config.slot_size ||
        !IsSupportedDescriptor(descriptor) ||
        !pool->ImportBufferStrict(static_cast<int>(i),
                                  descriptor.nvsci_buf_ipc_desc)) {
      return IsSupportedDescriptor(descriptor)
                 ? GpuChannelIpcStatus::kImportFailed
                 : GpuChannelIpcStatus::kUnsupported;
    }
  }

  const uint64_t channel_id = static_cast<uint64_t>(common::Hash(channel_name));
  const uint64_t engine_id = sync_engine_id != 0 ? sync_engine_id : channel_id;
  auto sync_engine = std::make_shared<NvSciSyncEngine>(engine_id);
  if (!producer_sync_desc.empty()) {
    if (!sync_engine->ImportSyncObj(producer_sync_desc)) {
      return GpuChannelIpcStatus::kImportFailed;
    }
  }
  sessions_[channel_name] =
      std::make_shared<GpuChannelSession>(channel_id, pool, sync_engine);
  return GpuChannelIpcStatus::kSuccess;
}

GpuChannelIpcStatus GpuChannelManager::ImportSession(
    const std::string& channel_name, const GpuSessionDescriptor& descriptor,
    uint64_t sync_engine_id, bool replace_existing) {
  if (channel_name.empty() || descriptor.channel_id == 0 ||
      descriptor.session_id == 0 || descriptor.producer_sync_desc.empty() ||
      descriptor.buffers.empty() ||
      descriptor.buffers.size() != descriptor.config.slot_count) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }
  const uint64_t expected_channel =
      static_cast<uint64_t>(common::Hash(channel_name));
  if (descriptor.channel_id != expected_channel) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto existing = sessions_.find(channel_name);
  if (existing != sessions_.end() && !replace_existing) {
    return GpuChannelIpcStatus::kSessionAlreadyExists;
  }
  if (existing != sessions_.end() &&
      existing->second->session_id() == descriptor.session_id) {
    return GpuChannelIpcStatus::kSuccess;
  }

  auto pool = std::make_shared<NvSciBufPool>(descriptor.config);
  if (!pool->Initialize()) {
    return GpuChannelIpcStatus::kImportFailed;
  }
  for (size_t i = 0; i < descriptor.buffers.size(); ++i) {
    const auto& buffer = descriptor.buffers[i];
    if (buffer.slot_id != i || buffer.capacity != descriptor.config.slot_size ||
        !IsSupportedDescriptor(buffer) ||
        !pool->ImportBufferStrict(static_cast<int>(i),
                                  buffer.nvsci_buf_ipc_desc)) {
      return IsSupportedDescriptor(buffer) ? GpuChannelIpcStatus::kImportFailed
                                           : GpuChannelIpcStatus::kUnsupported;
    }
  }
  const uint64_t engine_id = sync_engine_id != 0
                                 ? sync_engine_id
                                 : expected_channel ^ descriptor.session_id;
  auto sync_engine = std::make_shared<NvSciSyncEngine>(engine_id);
  if (!sync_engine->ImportSyncObj(descriptor.producer_sync_desc)) {
    return GpuChannelIpcStatus::kImportFailed;
  }
  sessions_[channel_name] = std::make_shared<GpuChannelSession>(
      expected_channel, pool, sync_engine, descriptor.session_id);
  return GpuChannelIpcStatus::kSuccess;
}

void GpuChannelManager::RemoveSession(const std::string& channel_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(channel_name);
}

void GpuChannelManager::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.clear();
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

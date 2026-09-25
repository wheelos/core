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

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "cyber/common/util.h"
#include "cyber/transport/nvsci/gpu_wire.h"

namespace apollo {
namespace cyber {
namespace transport {
namespace {

std::atomic<uint64_t> g_session_sequence{1};

void CloseWriterLock(int lock_fd) {
  if (lock_fd >= 0 && ::close(lock_fd) != 0) {
    AERROR << "Failed to close GPU writer lock: " << std::strerror(errno);
  }
}

bool IsSafeDirectory(int fd, mode_t forbidden_permissions) {
  struct stat info{};
  return ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) &&
         info.st_uid == ::geteuid() &&
         (info.st_mode & forbidden_permissions) == 0;
}

int AcquireWriterLock(const std::string& channel_name) {
  const char* base = std::getenv("XDG_RUNTIME_DIR");
  if (base == nullptr || *base == '\0') {
    base = std::getenv("HOME");
  }
  if (base == nullptr || *base != '/') {
    AERROR << "GPU writer lock requires an absolute XDG_RUNTIME_DIR or HOME";
    return -1;
  }
  const int base_fd =
      ::open(base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (base_fd < 0 || !IsSafeDirectory(base_fd, S_IWGRP | S_IWOTH)) {
    AERROR << "Unsafe GPU writer lock base directory " << base;
    CloseWriterLock(base_fd);
    return -1;
  }
  constexpr char kLockDirectory[] = "cyber_gpu_writer";
  if (::mkdirat(base_fd, kLockDirectory, 0700) != 0 && errno != EEXIST) {
    const int error = errno;
    CloseWriterLock(base_fd);
    AERROR << "Failed to create GPU writer lock directory: "
           << std::strerror(error);
    return -1;
  }
  const int dir_fd = ::openat(base_fd, kLockDirectory,
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  CloseWriterLock(base_fd);
  if (dir_fd < 0 || !IsSafeDirectory(dir_fd, S_IRWXG | S_IRWXO)) {
    AERROR << "Unsafe GPU writer lock directory under " << base;
    CloseWriterLock(dir_fd);
    return -1;
  }

  const std::string lock_name =
      std::to_string(common::Hash(channel_name)) + ".lock";
  const int lock_fd = ::openat(dir_fd, lock_name.c_str(),
                               O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600);
  const int open_error = errno;
  CloseWriterLock(dir_fd);
  if (lock_fd < 0) {
    AERROR << "Failed to open GPU writer lock for " << channel_name << ": "
           << std::strerror(open_error);
    return -1;
  }

  struct stat lock_stat{};
  if (::fstat(lock_fd, &lock_stat) != 0) {
    const int error = errno;
    CloseWriterLock(lock_fd);
    AERROR << "Failed to inspect GPU writer lock for " << channel_name << ": "
           << std::strerror(error);
    return -1;
  }
  if (!S_ISREG(lock_stat.st_mode) || lock_stat.st_uid != ::geteuid() ||
      lock_stat.st_nlink != 1 ||
      (lock_stat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    CloseWriterLock(lock_fd);
    AERROR << "Unsafe GPU writer lock file for " << channel_name;
    return -1;
  }

  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    const int error = errno;
    CloseWriterLock(lock_fd);
    if (error == EWOULDBLOCK || error == EAGAIN) {
      AWARN << "GPU channel " << channel_name
            << " already has a writer in another process";
    } else {
      AERROR << "Failed to acquire GPU writer lock for " << channel_name << ": "
             << std::strerror(error);
    }
    return -1;
  }
  return lock_fd;
}

uint64_t NextSessionId() {
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::system_clock::now().time_since_epoch().count());
  const uint64_t sequence =
      g_session_sequence.fetch_add(1, std::memory_order_relaxed);
  uint64_t id = now ^ (static_cast<uint64_t>(getpid()) << 32) ^ sequence;
  return id == 0 ? sequence : id;
}

GpuBufferBackend DetectDescriptorBackend(
    const GpuBufferDescriptor& descriptor) {
  if (descriptor.nvsci_buf_ipc_desc.size() >= kGpuBufferDescriptorHeaderSize) {
    size_t offset = 0;
    uint32_t magic = 0;
    if (wire::ReadU32(descriptor.nvsci_buf_ipc_desc.data(),
                      descriptor.nvsci_buf_ipc_desc.size(), &offset, &magic)) {
      if (magic == kCudaIpcBufferMagic) {
        return GpuBufferBackend::CUDA_IPC;
      }
      if (magic == kOrinUmaBufferMagic) {
        return GpuBufferBackend::ORIN_UMA;
      }
    }
  }
  return GpuBufferBackend::UNKNOWN;
}

bool IsSupportedDescriptor(const GpuBufferDescriptor& descriptor) {
  const auto backend = DetectDescriptorBackend(descriptor);
  if (descriptor.backend != GpuBufferBackend::UNKNOWN &&
      descriptor.backend != backend) {
    return false;
  }
  if (backend == GpuBufferBackend::CUDA_IPC ||
      backend == GpuBufferBackend::ORIN_UMA) {
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(channel_name);
    if (it != sessions_.end()) {
      return it->second;
    }
  }

  uint64_t channel_id = static_cast<uint64_t>(common::Hash(channel_name));
  auto pool = std::make_shared<NvSciBufPool>(config);
  if (!pool->Initialize()) {
    return nullptr;
  }

  uint64_t engine_id = sync_engine_id != 0 ? sync_engine_id : channel_id;
  auto sync_engine = std::make_shared<NvSciSyncEngine>(
      engine_id, pool->GetBackend() == GpuBufferBackend::ORIN_UMA);
  auto session = std::make_shared<GpuChannelSession>(
      channel_id, pool, sync_engine, NextSessionId());

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(channel_name);
  if (it != sessions_.end()) {
    return it->second;
  }
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
  exported.backend = session->pool()->GetBackend();
  if (!session->sync_engine()->ExportSyncObj(&exported.producer_sync_desc)) {
    return GpuChannelIpcStatus::kUnsupported;
  }
  const auto status = ExportSession(channel_name, &exported.buffers);
  if (status != GpuChannelIpcStatus::kSuccess) {
    return status;
  }
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

bool GpuChannelManager::ClaimWriterSession(
    const std::string& channel_name, const GpuChannelSessionPtr& session) {
  if (channel_name.empty() || !session) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current = sessions_.find(channel_name);
  if (current == sessions_.end() || current->second != session ||
      writer_sessions_.find(channel_name) != writer_sessions_.end()) {
    return false;
  }
  const int lock_fd = AcquireWriterLock(channel_name);
  if (lock_fd < 0) {
    return false;
  }
  writer_sessions_.emplace(channel_name,
                           WriterSessionLease{session->session_id(), lock_fd});
  return true;
}

void GpuChannelManager::ReleaseWriterSession(const std::string& channel_name,
                                             uint64_t session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto owner = writer_sessions_.find(channel_name);
  if (owner == writer_sessions_.end() ||
      owner->second.session_id != session_id) {
    return;
  }
  CloseWriterLock(owner->second.lock_fd);
  writer_sessions_.erase(owner);
  const auto session = sessions_.find(channel_name);
  if (session != sessions_.end() &&
      session->second->session_id() == session_id) {
    sessions_.erase(session);
  }
  latest_import_tokens_.erase(channel_name);
}

void GpuChannelManager::DiscardUnclaimedSession(
    const std::string& channel_name, const GpuChannelSessionPtr& session) {
  if (channel_name.empty() || !session) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current = sessions_.find(channel_name);
  if (current == sessions_.end() || current->second != session ||
      writer_sessions_.find(channel_name) != writer_sessions_.end() ||
      session->GetConsumerCount() != 0) {
    return;
  }
  sessions_.erase(current);
  latest_import_tokens_.erase(channel_name);
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
    descriptor.backend = DetectDescriptorBackend(descriptor);
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
    uint64_t sync_engine_id, const std::vector<uint8_t>& producer_sync_desc) {
  if (channel_name.empty() || descriptors.empty() ||
      descriptors.size() != config.slot_count) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(channel_name) != sessions_.end()) {
      return GpuChannelIpcStatus::kSessionAlreadyExists;
    }
  }

  GpuChannelSessionPtr imported_session;
  const auto import_status =
      CreateImportedSession(channel_name, config, descriptors, sync_engine_id,
                            producer_sync_desc, &imported_session);
  if (import_status != GpuChannelIpcStatus::kSuccess) {
    return import_status;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (sessions_.find(channel_name) != sessions_.end()) {
    return GpuChannelIpcStatus::kSessionAlreadyExists;
  }
  sessions_[channel_name] = std::move(imported_session);
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

  uint64_t import_token = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = sessions_.find(channel_name);
    if (existing != sessions_.end()) {
      if (!replace_existing) {
        return GpuChannelIpcStatus::kSessionAlreadyExists;
      }
      if (existing->second->session_id() == descriptor.session_id) {
        return GpuChannelIpcStatus::kSuccess;
      }
    }
    import_token = next_import_token_++;
    latest_import_tokens_[channel_name] = import_token;
  }

  GpuChannelSessionPtr imported_session;
  const auto import_status = CreateImportedSession(
      channel_name, descriptor, sync_engine_id, &imported_session);
  if (import_status != GpuChannelIpcStatus::kSuccess) {
    return import_status;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto latest_import = latest_import_tokens_.find(channel_name);
  if (latest_import == latest_import_tokens_.end() ||
      latest_import->second != import_token) {
    return GpuChannelIpcStatus::kSessionAlreadyExists;
  }
  auto existing = sessions_.find(channel_name);
  if (existing != sessions_.end() && !replace_existing) {
    return GpuChannelIpcStatus::kSessionAlreadyExists;
  }
  sessions_[channel_name] = std::move(imported_session);
  latest_import_tokens_.erase(latest_import);
  return GpuChannelIpcStatus::kSuccess;
}

GpuChannelIpcStatus GpuChannelManager::CreateImportedSession(
    const std::string& channel_name, const GpuSessionDescriptor& descriptor,
    uint64_t sync_engine_id, GpuChannelSessionPtr* session) const {
  if (session == nullptr || channel_name.empty() ||
      descriptor.channel_id == 0 || descriptor.session_id == 0 ||
      descriptor.producer_sync_desc.empty() || descriptor.buffers.empty() ||
      descriptor.buffers.size() != descriptor.config.slot_count) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }
  const uint64_t expected_channel =
      static_cast<uint64_t>(common::Hash(channel_name));
  if (descriptor.channel_id != expected_channel) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }

  auto pool = std::make_shared<NvSciBufPool>(descriptor.config);
  if (!pool->Initialize()) {
    return GpuChannelIpcStatus::kImportFailed;
  }
  for (size_t i = 0; i < descriptor.buffers.size(); ++i) {
    const auto& buffer = descriptor.buffers[i];
    if (buffer.slot_id != i || buffer.capacity != descriptor.config.slot_size ||
        buffer.backend != descriptor.backend ||
        DetectDescriptorBackend(buffer) != descriptor.backend ||
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
  auto sync_engine = std::make_shared<NvSciSyncEngine>(
      engine_id, descriptor.backend == GpuBufferBackend::ORIN_UMA);
  if (!sync_engine->ImportSyncObj(descriptor.producer_sync_desc)) {
    return GpuChannelIpcStatus::kImportFailed;
  }
  *session = std::make_shared<GpuChannelSession>(
      expected_channel, pool, sync_engine, descriptor.session_id);
  return GpuChannelIpcStatus::kSuccess;
}

GpuChannelIpcStatus GpuChannelManager::CreateImportedSession(
    const std::string& channel_name, const NvSciBufPoolConfig& config,
    const std::vector<GpuBufferDescriptor>& descriptors,
    uint64_t sync_engine_id, const std::vector<uint8_t>& producer_sync_desc,
    GpuChannelSessionPtr* session) const {
  if (session == nullptr || channel_name.empty() || descriptors.empty() ||
      descriptors.size() != config.slot_count) {
    return GpuChannelIpcStatus::kInvalidArgument;
  }

  auto pool = std::make_shared<NvSciBufPool>(config);
  if (!pool->Initialize()) {
    return GpuChannelIpcStatus::kImportFailed;
  }
  for (size_t i = 0; i < descriptors.size(); ++i) {
    const auto& descriptor = descriptors[i];
    const auto actual_backend = DetectDescriptorBackend(descriptor);
    if (descriptor.slot_id != i || descriptor.capacity != config.slot_size ||
        (i > 0 &&
         actual_backend != DetectDescriptorBackend(descriptors.front())) ||
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
  const bool host_synchronized =
      !descriptors.empty() && DetectDescriptorBackend(descriptors.front()) ==
                                  GpuBufferBackend::ORIN_UMA;
  auto sync_engine =
      std::make_shared<NvSciSyncEngine>(engine_id, host_synchronized);
  if (!producer_sync_desc.empty() &&
      !sync_engine->ImportSyncObj(producer_sync_desc)) {
    return GpuChannelIpcStatus::kImportFailed;
  }
  *session = std::make_shared<GpuChannelSession>(channel_id, pool, sync_engine);
  return GpuChannelIpcStatus::kSuccess;
}

void GpuChannelManager::RemoveSession(const std::string& channel_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(channel_name);
  const auto owner = writer_sessions_.find(channel_name);
  if (owner != writer_sessions_.end()) {
    CloseWriterLock(owner->second.lock_fd);
    writer_sessions_.erase(owner);
  }
  latest_import_tokens_.erase(channel_name);
}

void GpuChannelManager::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.clear();
  for (const auto& writer : writer_sessions_) {
    CloseWriterLock(writer.second.lock_fd);
  }
  writer_sessions_.clear();
  latest_import_tokens_.clear();
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

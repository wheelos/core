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

#ifndef CYBER_TRANSPORT_NVSCI_GPU_CHANNEL_MANAGER_H_
#define CYBER_TRANSPORT_NVSCI_GPU_CHANNEL_MANAGER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cyber/common/macros.h"
#include "cyber/transport/nvsci/gpu_channel_session.h"
#include "cyber/transport/nvsci/nvsci_buf_pool.h"
#include "cyber/transport/nvsci/nvsci_sync_engine.h"

namespace apollo {
namespace cyber {
namespace transport {

enum class GpuChannelIpcStatus : uint8_t {
  kSuccess = 0,
  kInvalidArgument,
  kSessionNotFound,
  kSessionAlreadyExists,
  kUnsupported,
  kExportFailed,
  kImportFailed,
};

class GpuChannelManager {
 public:
  static GpuChannelManager* Instance();

  GpuChannelSessionPtr GetOrCreateSession(const std::string& channel_name,
                                          const NvSciBufPoolConfig& config,
                                          uint64_t sync_engine_id = 0);

  GpuChannelSessionPtr GetSession(const std::string& channel_name) const;

  bool ClaimWriterSession(const std::string& channel_name,
                          const GpuChannelSessionPtr& session);
  void ReleaseWriterSession(const std::string& channel_name,
                            uint64_t session_id);

  // Exports the pool's CUDA IPC handles for one-time cross-process setup.
  // This deliberately rejects the host-memory simulation and NvSci fallback:
  // those descriptors cannot safely be used by a separate process.
  GpuChannelIpcStatus ExportSession(
      const std::string& channel_name,
      std::vector<GpuBufferDescriptor>* descriptors) const;
  GpuChannelIpcStatus ExportSession(const std::string& channel_name,
                                    GpuSessionDescriptor* descriptor) const;

  // Installs a reader-side session from descriptors produced by ExportSession.
  // The imported session is process-local, while its slots refer to the
  // producer's CUDA allocations.
  GpuChannelIpcStatus ImportSession(
      const std::string& channel_name, const NvSciBufPoolConfig& config,
      const std::vector<GpuBufferDescriptor>& descriptors,
      uint64_t sync_engine_id = 0,
      const std::vector<uint8_t>& producer_sync_desc = {});
  GpuChannelIpcStatus ImportSession(const std::string& channel_name,
                                    const GpuSessionDescriptor& descriptor,
                                    uint64_t sync_engine_id = 0,
                                    bool replace_existing = false);

  GpuChannelIpcStatus CreateImportedSession(
      const std::string& channel_name, const GpuSessionDescriptor& descriptor,
      uint64_t sync_engine_id, GpuChannelSessionPtr* session) const;
  GpuChannelIpcStatus CreateImportedSession(
      const std::string& channel_name, const NvSciBufPoolConfig& config,
      const std::vector<GpuBufferDescriptor>& descriptors,
      uint64_t sync_engine_id, const std::vector<uint8_t>& producer_sync_desc,
      GpuChannelSessionPtr* session) const;

  void RemoveSession(const std::string& channel_name);

  void Clear();

 private:
  GpuChannelManager() = default;
  ~GpuChannelManager() = default;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, GpuChannelSessionPtr> sessions_;
  std::unordered_map<std::string, uint64_t> writer_sessions_;
  std::unordered_map<std::string, uint64_t> latest_import_tokens_;
  uint64_t next_import_token_ = 1;

  DISALLOW_COPY_AND_ASSIGN(GpuChannelManager);
};

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_GPU_CHANNEL_MANAGER_H_

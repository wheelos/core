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

#include "cyber/transport/nvsci/orin_nvsci_backend.h"

#include <chrono>
#include <cstring>

namespace apollo {
namespace cyber {
namespace transport {
namespace orin {

bool OrinNvSciBackend::InitializeModule() {
#if defined(CYBER_USE_NVSCI)
  // On NVIDIA Jetson AGX Orin / DRIVE OS:
  // NvSciBufModuleOpen(&buf_module);
  // NvSciSyncModuleOpen(&sync_module);
  return true;
#else
  return true;
#endif
}

void OrinNvSciBackend::DeinitializeModule() {
#if defined(CYBER_USE_NVSCI)
  // NvSciBufModuleClose(buf_module);
  // NvSciSyncModuleClose(sync_module);
#endif
}

bool OrinNvSciBackend::AllocateNvSciBuffer(size_t size, uint32_t alignment,
                                          void** out_dev_ptr,
                                          std::vector<uint8_t>* out_ipc_desc) {
  if (!out_dev_ptr || !out_ipc_desc || size == 0) {
    return false;
  }

#if defined(CYBER_USE_NVSCI)
  // Real Orin workflow requires target NvSci SDK and DriveOS libraries linked:
  // 1. NvSciBufAttrListCreate
  // 2. Set CPU & GPU raw buffer attributes
  // 3. NvSciBufAttrListReconcile
  // 4. NvSciBufObjAlloc(reconciledList, &bufObj)
  // 5. cudaImportExternalMemory + cudaExternalMemoryGetMappedBuffer
  // 6. NvSciBufObjExportIpcMemHandle
  AERROR << "NvSciBufObjAlloc requested on target without hardware NvSci SDK linked";
  *out_dev_ptr = nullptr;
  out_ipc_desc->clear();
  return false;
#else
  (void)alignment;
  // Fallback descriptor for non-NvSci platform build
  out_ipc_desc->resize(24);
  const uint64_t magic = 0x4F52494E53434931ULL;  // "ORINSCI1"
  uint64_t sz = static_cast<uint64_t>(size);
  std::memcpy(out_ipc_desc->data(), &magic, sizeof(magic));
  std::memcpy(out_ipc_desc->data() + 8, &sz, sizeof(sz));
  return true;
#endif
}

bool OrinNvSciBackend::ImportNvSciBuffer(const std::vector<uint8_t>& ipc_desc,
                                        void** out_dev_ptr) {
  if (!out_dev_ptr || ipc_desc.empty()) {
    return false;
  }

#if defined(CYBER_USE_NVSCI)
  // Real Orin workflow requires target NvSci SDK:
  // 1. NvSciBufObjImportIpcMemHandle
  // 2. cudaImportExternalMemory + cudaExternalMemoryGetMappedBuffer
  AERROR << "NvSciBufObjImport requested on target without hardware NvSci SDK linked";
  *out_dev_ptr = nullptr;
  return false;
#else
  const uint64_t magic = 0x4F52494E53434931ULL;
  if (ipc_desc.size() < 16) {
    return false;
  }
  uint64_t parsed_magic = 0;
  std::memcpy(&parsed_magic, ipc_desc.data(), sizeof(parsed_magic));
  return parsed_magic == magic;
#endif
}

void OrinNvSciBackend::FreeNvSciBuffer(void* dev_ptr) {
  if (!dev_ptr) {
    return;
  }
#if defined(CYBER_USE_NVSCI)
  // cudaDestroyExternalMemory / NvSciBufObjFree
#endif
}

bool OrinNvSciBackend::GenerateNvSciFence(void* stream_ptr,
                                         NvSciSyncFence* out_fence) {
  if (!out_fence) {
    return false;
  }
#if defined(CYBER_USE_NVSCI)
  // Real Orin workflow:
  // cudaExternalSemaphore_t extSem;
  // cudaExternalSemaphoreSignalParams signalParams{};
  // cudaSignalExternalSemaphoresAsync(&extSem, &signalParams, 1, stream);
  // NvSciSyncObjGenerateFence(syncObj, reinterpret_cast<NvSciSyncFence*>(out_fence));
  out_fence->Reset();
  AERROR << "GenerateNvSciFence requested on target without hardware NvSci SDK linked";
  return false;
#else
  (void)stream_ptr;
  out_fence->fence_id = 1;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  out_fence->timestamp_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  const uint32_t magic = 0x4F52494E;  // "ORIN"
  std::memcpy(out_fence->payload.data(), &magic, sizeof(magic));
  return true;
#endif
}

bool OrinNvSciBackend::InsertWaitNvSciFence(void* stream_ptr,
                                           const NvSciSyncFence& fence) {
  if (!fence.IsValid()) {
    return false;
  }
#if defined(CYBER_USE_NVSCI)
  // Real Orin workflow:
  // cudaExternalSemaphoreWaitParams waitParams{};
  // cudaWaitExternalSemaphoresAsync(&extSem, &waitParams, 1, stream);
  AERROR << "InsertWaitNvSciFence requested on target without hardware NvSci SDK linked";
  return false;
#else
  (void)stream_ptr;
  return true;
#endif
}

}  // namespace orin
}  // namespace transport
}  // namespace cyber
}  // namespace apollo

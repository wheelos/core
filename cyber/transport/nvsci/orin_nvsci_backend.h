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

#ifndef CYBER_TRANSPORT_NVSCI_ORIN_NVSCI_BACKEND_H_
#define CYBER_TRANSPORT_NVSCI_ORIN_NVSCI_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cyber/common/log.h"
#include "cyber/transport/nvsci/nvsci_types.h"
#include "cyber/transport/nvsci/orin_nvsci_wrapper.h"

namespace apollo {
namespace cyber {
namespace transport {
namespace orin {

/**
 * @brief Orin NvSciBuf and NvSciSync Hardware Backend implementation.
 *
 * Implements Jetson Orin NvSciBuf object allocation, attribute reconciliation,
 * CUDA external memory mapping (cudaImportExternalMemory / cudaExternalMemoryGetMappedBuffer),
 * NvSciSync object allocation, and hardware fence generation/waiting.
 */
class OrinNvSciBackend {
 public:
  static bool InitializeModule();
  static void DeinitializeModule();

  static bool AllocateNvSciBuffer(size_t size, uint32_t alignment,
                                  void** out_dev_ptr,
                                  std::vector<uint8_t>* out_ipc_desc);

  static bool ImportNvSciBuffer(const std::vector<uint8_t>& ipc_desc,
                                void** out_dev_ptr);

  static void FreeNvSciBuffer(void* dev_ptr);

  static bool GenerateNvSciFence(void* stream_ptr, NvSciSyncFence* out_fence);
  static bool InsertWaitNvSciFence(void* stream_ptr,
                                   const NvSciSyncFence& fence);
};

}  // namespace orin
}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_ORIN_NVSCI_BACKEND_H_

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

#ifndef CYBER_TRANSPORT_NVSCI_ORIN_NVSCI_WRAPPER_H_
#define CYBER_TRANSPORT_NVSCI_ORIN_NVSCI_WRAPPER_H_

#include <cstddef>
#include <cstdint>

#if defined(CYBER_USE_NVSCI)
#include <nvscibuf.h>
#include <nvscisync.h>
#include <cuda_runtime.h>
#endif

namespace apollo {
namespace cyber {
namespace transport {
namespace orin {

#if !defined(CYBER_USE_NVSCI)
// Authentic NvSci and CUDA interop types emulation for platforms compiling
// without NvSci SDK (e.g. x86 compilation or cross-compilation testing).
// When CYBER_USE_NVSCI is enabled on NVIDIA DRIVE/Jetson Orin, the real
// nvscibuf.h and nvscisync.h headers are included above.

typedef void* NvSciBufModule;
typedef void* NvSciBufAttrList;
typedef void* NvSciBufObj;
typedef void* NvSciSyncModule;
typedef void* NvSciSyncAttrList;
typedef void* NvSciSyncObj;

enum NvSciError {
  NvSciError_Success = 0,
  NvSciError_BadParameter = 1,
  NvSciError_InsufficientMemory = 2,
  NvSciError_NotInitialized = 3,
  NvSciError_ResourceError = 4,
  NvSciError_NotSupported = 5,
};

enum NvSciBufAttrKey {
  NvSciBufGeneralAttrKey_Types = 0,
  NvSciBufRawBufferAttrKey_Size = 1,
  NvSciBufRawBufferAttrKey_Align = 2,
};

enum NvSciBufType {
  NvSciBufType_RawBuffer = 0,
};

enum NvSciSyncAttrKey {
  NvSciSyncAttrKey_RequirePerf = 0,
};

enum NvSciSyncAccessPerm {
  NvSciSyncAccessPerm_WaitOnly = 1,
  NvSciSyncAccessPerm_SignalOnly = 2,
  NvSciSyncAccessPerm_WaitSignal = 3,
};
#endif

}  // namespace orin
}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_ORIN_NVSCI_WRAPPER_H_

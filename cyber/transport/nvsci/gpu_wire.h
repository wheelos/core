/******************************************************************************
 * Copyright 2026 WheelOS. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *****************************************************************************/

#ifndef CYBER_TRANSPORT_NVSCI_GPU_WIRE_H_
#define CYBER_TRANSPORT_NVSCI_GPU_WIRE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace apollo {
namespace cyber {
namespace transport {
namespace wire {

inline void AppendU32(uint32_t value, std::string* out) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<char>((value >> (i * 8)) & 0xffU));
  }
}

inline void AppendU64(uint64_t value, std::string* out) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((value >> (i * 8)) & 0xffU));
  }
}

inline void AppendU32(uint32_t value, std::vector<uint8_t>* out) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffU));
  }
}

inline void AppendU64(uint64_t value, std::vector<uint8_t>* out) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffU));
  }
}

inline bool ReadU32(const uint8_t* data, size_t size, size_t* offset,
                    uint32_t* value) {
  if (data == nullptr || offset == nullptr || value == nullptr ||
      *offset > size || size - *offset < 4) {
    return false;
  }
  *value = 0;
  for (int i = 0; i < 4; ++i) {
    *value |= static_cast<uint32_t>(data[*offset + i]) << (i * 8);
  }
  *offset += 4;
  return true;
}

inline bool ReadU64(const uint8_t* data, size_t size, size_t* offset,
                    uint64_t* value) {
  if (data == nullptr || offset == nullptr || value == nullptr ||
      *offset > size || size - *offset < 8) {
    return false;
  }
  *value = 0;
  for (int i = 0; i < 8; ++i) {
    *value |= static_cast<uint64_t>(data[*offset + i]) << (i * 8);
  }
  *offset += 8;
  return true;
}

}  // namespace wire
}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_NVSCI_GPU_WIRE_H_

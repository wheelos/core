/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
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

#ifndef CYBER_MESSAGE_FLATBUFFERS_MESSAGE_H_
#define CYBER_MESSAGE_FLATBUFFERS_MESSAGE_H_

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "flatbuffers/flatbuffers.h"

namespace apollo {
namespace cyber {
namespace message {

class FlatBufferMessage {
 public:
  FlatBufferMessage() : type_name_(""), timestamp_(0) {}

  FlatBufferMessage(const std::string& type_name, const uint8_t* data,
                    size_t size)
      : type_name_(type_name), buffer_(data, data + size), timestamp_(0) {}

  FlatBufferMessage(const FlatBufferMessage&) = default;
  FlatBufferMessage& operator=(const FlatBufferMessage&) = default;
  ~FlatBufferMessage() = default;

  class Descriptor {
   public:
    std::string full_name() const {
      return "apollo.cyber.message.FlatBufferMessage";
    }
    std::string name() const {
      return "apollo.cyber.message.FlatBufferMessage";
    }
  };

  static const Descriptor* descriptor() {
    static Descriptor desc;
    return &desc;
  }

  static std::string TypeName() {
    return "apollo.cyber.message.FlatBufferMessage";
  }

  bool SerializeToArray(void* data, int size) const {
    if (data == nullptr || size < ByteSize()) {
      return false;
    }
    std::memcpy(data, buffer_.data(), buffer_.size());
    return true;
  }

  bool SerializeToString(std::string* str) const {
    if (str == nullptr) {
      return false;
    }
    str->assign(reinterpret_cast<const char*>(buffer_.data()), buffer_.size());
    return true;
  }

  bool ParseFromArray(const void* data, int size) {
    if (data == nullptr || size <= 0) {
      return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    buffer_.assign(bytes, bytes + size);
    return true;
  }

  bool ParseFromString(const std::string& str) {
    buffer_.assign(str.begin(), str.end());
    return true;
  }

  int ByteSize() const { return static_cast<int>(buffer_.size()); }
  size_t ByteSizeLong() const { return buffer_.size(); }

  const uint8_t* data() const {
    return buffer_.empty() ? nullptr : buffer_.data();
  }

  size_t size() const { return buffer_.size(); }

  template <typename T>
  const T* GetRoot() const {
    if (buffer_.empty()) {
      return nullptr;
    }
    return flatbuffers::GetRoot<T>(buffer_.data());
  }

  const std::string& type_name() const { return type_name_; }
  void set_type_name(const std::string& name) { type_name_ = name; }
  void SetTypeName(const std::string& name) { type_name_ = name; }

  uint64_t timestamp() const { return timestamp_; }
  void set_timestamp(uint64_t ts) { timestamp_ = ts; }

 private:
  std::string type_name_;
  std::vector<uint8_t> buffer_;
  uint64_t timestamp_;
};

}  // namespace message
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_MESSAGE_FLATBUFFERS_MESSAGE_H_

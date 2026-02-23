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

#include "flatbuffers/flatbuffers.h"

namespace apollo {
namespace cyber {
namespace message {

/**
 * @brief Zero-copy message wrapper for FlatBuffers-serialized data.
 *
 * FlatBufferMessage stores a serialized FlatBuffers byte buffer and exposes
 * the standard Cyber RT message interface (SerializeToArray, ParseFromArray,
 * etc.) so it can be used interchangeably with RawMessage or Protobuf messages
 * in the Cyber transport layer.
 *
 * Zero-copy semantics: when data arrives via the shared-memory transport, the
 * underlying buffer pointer can be accessed directly through GetRoot<T>()
 * without any deserialization step.
 *
 * Usage example:
 * @code
 *   flatbuffers::FlatBufferBuilder fbb;
 *   auto content = fbb.CreateString("Hello FlatBuffers");
 *   auto chatter = CreateChatter(fbb, 123, 456, 1, content);
 *   fbb.Finish(chatter);
 *
 *   FlatBufferMessage msg("my.type", fbb.GetBufferPointer(), fbb.GetSize());
 *
 *   // Zero-copy read:
 *   const auto* root = msg.GetRoot<Chatter>();
 * @endcode
 */
class FlatBufferMessage {
 public:
  FlatBufferMessage() : type_name_(""), timestamp_(0) {}

  /**
   * @brief Construct from a type name and a pre-built FlatBuffers byte buffer.
   *
   * @param type_name  Logical type identifier (used by the transport layer).
   * @param data       Pointer to the FlatBuffers serialized bytes.
   * @param size       Number of bytes in @p data.
   */
  FlatBufferMessage(const std::string& type_name, const uint8_t* data,
                    size_t size)
      : type_name_(type_name),
        buffer_(data, data + size),
        timestamp_(0) {}

  FlatBufferMessage(const FlatBufferMessage&) = default;
  FlatBufferMessage& operator=(const FlatBufferMessage&) = default;
  ~FlatBufferMessage() = default;

  // ----- Descriptor / type identity (mirrors RawMessage interface) ----------

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

  // ----- Serialization interface (required by message_traits.h) -------------

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

  // ----- Zero-copy accessors ------------------------------------------------

  /**
   * @brief Return a pointer to the raw serialized bytes.
   *
   * When the transport layer writes this buffer directly into shared memory,
   * the receiver can call GetRoot<T>() on the same pointer without copying.
   */
  const uint8_t* data() const { return buffer_.data(); }

  size_t size() const { return buffer_.size(); }

  /**
   * @brief Zero-copy typed access to the FlatBuffers root object.
   *
   * @tparam T  The FlatBuffers-generated root table type.
   * @return    Pointer into the existing buffer — no copy, no allocation.
   */
  template <typename T>
  const T* GetRoot() const {
    if (buffer_.empty()) {
      return nullptr;
    }
    return flatbuffers::GetRoot<T>(buffer_.data());
  }

  // ----- Ancillary fields used by the transport layer -----------------------

  const std::string& type_name() const { return type_name_; }

  /**
   * @brief Lowercase setter following conventional C++ naming style.
   */
  void set_type_name(const std::string& name) { type_name_ = name; }

  /**
   * @brief UpperCamelCase setter required by the `HasSetType` trait in
   * message_traits.h.
   *
   * Both `set_type_name()` and `SetTypeName()` exist intentionally:
   * `set_type_name()` follows standard C++ getter/setter naming, while
   * `SetTypeName()` is the name detected by `DEFINE_TYPE_TRAIT(HasSetType,
   * SetTypeName)` so the transport layer (e.g., `ParseFromHC`) can propagate
   * the message type identifier through a generic template interface.
   */
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

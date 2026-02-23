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

#ifndef CYBER_MESSAGE_FLATBUFFERS_TRAITS_H_
#define CYBER_MESSAGE_FLATBUFFERS_TRAITS_H_

#include <string>

#include "cyber/message/flatbuffers_message.h"

namespace apollo {
namespace cyber {
namespace message {

/**
 * Inline overloads for FlatBufferMessage.
 *
 * These follow the same pattern as raw_message_traits.h and
 * py_message_traits.h: plain (non-template) overloads that the compiler
 * selects ahead of the generic SFINAE templates in message_traits.h,
 * ensuring correct ByteSize / serialization behaviour for
 * FlatBufferMessage in the Cyber RT transport layer.
 */

inline bool SerializeToArray(const FlatBufferMessage& message, void* data,
                             int size) {
  return message.SerializeToArray(data, size);
}

inline bool ParseFromArray(const void* data, int size,
                           FlatBufferMessage* message) {
  return message->ParseFromArray(data, size);
}

/**
 * @brief Return the serialized byte size of a FlatBufferMessage.
 *
 * FlatBufferMessage::ByteSize() is the canonical size accessor.  This
 * overload ensures message::ByteSize(msg) returns the correct value for
 * the shared-memory transmitter (which would otherwise get -1 from the
 * generic SFINAE template that looks for ByteSizeLong()).
 */
inline int ByteSize(const FlatBufferMessage& message) {
  return message.ByteSize();
}

}  // namespace message
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_MESSAGE_FLATBUFFERS_TRAITS_H_

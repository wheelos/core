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

#ifndef CYBER_RECORD_FILE_RECORD_FILE_READER_H_
#define CYBER_RECORD_FILE_RECORD_FILE_READER_H_

#include <liburing.h>
#include <sys/uio.h>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

#include "cyber/common/log.h"
#include "cyber/record/file/record_file_base.h"
#include "cyber/record/file/section.h"

namespace apollo {
namespace cyber {
namespace record {

using google::protobuf::io::ArrayInputStream;
using google::protobuf::io::CodedInputStream;

class RecordFileReader : public RecordFileBase {
 public:
  RecordFileReader();
  virtual ~RecordFileReader();
  bool Open(const std::string& path) override;
  void Close() override;
  bool Reset();
  bool ReadSection(Section* section);
  bool SkipSection(int64_t size);
  template <typename T>
  bool ReadSection(int64_t size, T* message);
  bool ReadIndex();
  bool EndOfFile() const { return end_of_file_; }
  int64_t CurrentPosition() const { return static_cast<int64_t>(logical_offset_); }

 private:
  struct BufferSlot {
    char* data = nullptr;
    size_t capacity = 0;
    size_t valid_size = 0;
    uint64_t offset = 0;
    enum class State { FREE, IN_FLIGHT, READY };
    State state = State::FREE;
  };

  bool ReadHeader();
  bool SetPosition(uint64_t target_pos);
  bool ReadBytes(void* output, size_t size);
  bool EnsureCurrentSlot();
  bool PollCompletions(bool wait_for_one, bool* completed);
  bool SubmitRead(int slot_index, uint64_t offset);
  void FillReadWindow();
  void DrainInFlightReads();
  void ResetBufferState();
  void ReleaseBuffers();
  bool AcquireContiguousPayload(size_t size, const char** payload);
  void AdvanceLogicalOffset(size_t size);

  struct io_uring ring_ = {};
  bool ring_initialized_ = false;
  bool buffers_registered_ = false;
  std::vector<BufferSlot> slots_;
  std::vector<struct iovec> registered_iovecs_;
  std::map<uint64_t, int> ready_slots_by_offset_;
  int in_flight_reads_ = 0;
  uint64_t stream_base_offset_ = 0;
  uint64_t next_submit_offset_ = 0;
  uint64_t logical_offset_ = 0;
  int current_slot_index_ = -1;
  size_t current_slot_pos_ = 0;
  bool end_of_file_ = false;
  bool reached_physical_eof_ = false;
  std::vector<char> overflow_parse_buffer_;

  static constexpr size_t kSlotSize = 2 * 1024 * 1024;
  static constexpr int kSlotCount = 8;
  static constexpr int kPrefetchDepth = 4;
};

template <typename T>
bool RecordFileReader::ReadSection(int64_t size, T* message) {
  if (size < 0 || size > std::numeric_limits<int>::max()) {
    AERROR << "Invalid section payload size: " << size;
    return false;
  }
  const size_t payload_size = static_cast<size_t>(size);
  const char* payload = nullptr;
  if (AcquireContiguousPayload(payload_size, &payload)) {
    ArrayInputStream raw_input(payload, static_cast<int>(payload_size));
    if (!message->ParseFromZeroCopyStream(&raw_input)) {
      AERROR << "Parse section message failed.";
      return false;
    }
    AdvanceLogicalOffset(payload_size);
  } else {
    overflow_parse_buffer_.resize(payload_size);
    if (!ReadBytes(overflow_parse_buffer_.data(), payload_size)) {
      AERROR << "Read section payload failed, size: " << size;
      return false;
    }
    ArrayInputStream raw_input(overflow_parse_buffer_.data(),
                               static_cast<int>(payload_size));
    CodedInputStream coded_input(&raw_input);
    CodedInputStream::Limit limit = coded_input.PushLimit(static_cast<int>(size));
    if (!message->ParseFromCodedStream(&coded_input)) {
      AERROR << "Parse section message failed.";
      return false;
    }
    if (!coded_input.ConsumedEntireMessage()) {
      AERROR << "Do not consumed entire message.";
      return false;
    }
    coded_input.PopLimit(limit);
  }
  if (static_cast<int64_t>(message->ByteSizeLong()) != size) {
    AERROR << "Message size is not consistent in section header"
           << ", expect: " << size << ", actual: " << message->ByteSizeLong();
    return false;
  }
  return true;
}

}  // namespace record
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_RECORD_FILE_RECORD_FILE_READER_H_

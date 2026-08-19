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

#include "cyber/record/file/record_file_reader.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "cyber/common/file.h"
#include "cyber/task/task.h"

namespace apollo {
namespace cyber {
namespace record {

using apollo::cyber::proto::SectionType;

RecordFileReader::RecordFileReader() = default;

RecordFileReader::~RecordFileReader() { Close(); }

bool RecordFileReader::Open(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  path_ = path;
  if (!::apollo::cyber::common::PathExists(path_)) {
    AERROR << "File not exist, file: " << path_;
    return false;
  }
  fd_ = open(path_.data(), O_RDONLY);
  if (fd_ < 0) {
    AERROR << "Open file failed, file: " << path_ << ", fd: " << fd_
           << ", errno: " << errno;
    return false;
  }

  if (io_uring_queue_init(256, &ring_, 0) < 0) {
    AWARN << "io_uring_queue_init unavailable, using synchronous reads, file: "
          << path_;
  } else {
    ring_initialized_ = true;
  }

  slots_.resize(kSlotCount);
  registered_iovecs_.resize(kSlotCount);
  for (int i = 0; i < kSlotCount; ++i) {
    void* memory = nullptr;
    if (posix_memalign(&memory, 4096, kSlotSize) != 0) {
      AERROR << "Allocate slot memory failed, slot: " << i;
      Close();
      return false;
    }
    slots_[i].data = reinterpret_cast<char*>(memory);
    slots_[i].capacity = kSlotSize;
    slots_[i].state = BufferSlot::State::FREE;
    registered_iovecs_[i].iov_base = slots_[i].data;
    registered_iovecs_[i].iov_len = slots_[i].capacity;
  }
  if (ring_initialized_) {
    const int register_result =
        io_uring_register_buffers(&ring_, registered_iovecs_.data(),
                                  registered_iovecs_.size());
    if (register_result < 0) {
      AWARN << "io_uring_register_buffers unavailable, using regular reads, "
            << "file: " << path_ << ", errno: " << -register_result;
    } else {
      buffers_registered_ = true;
    }
  }

  if (!SetPosition(0)) {
    AERROR << "Initial read position setup failed, file: " << path_;
    return false;
  }
  if (!ReadHeader()) {
    AERROR << "Read header section fail, file: " << path_;
    return false;
  }
  return true;
}

void RecordFileReader::ReleaseBuffers() {
  if (ring_initialized_ && buffers_registered_) {
    io_uring_unregister_buffers(&ring_);
    buffers_registered_ = false;
  }
  for (auto& slot : slots_) {
    if (slot.data != nullptr) {
      free(slot.data);
      slot.data = nullptr;
    }
    slot.state = BufferSlot::State::FREE;
    slot.valid_size = 0;
  }
  slots_.clear();
  registered_iovecs_.clear();
  ready_slots_by_offset_.clear();
}

void RecordFileReader::Close() {
  DrainInFlightReads();
  ReleaseBuffers();
  if (ring_initialized_) {
    io_uring_queue_exit(&ring_);
    ring_initialized_ = false;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

void RecordFileReader::ResetBufferState() {
  ready_slots_by_offset_.clear();
  in_flight_reads_ = 0;
  current_slot_index_ = -1;
  current_slot_pos_ = 0;
  for (auto& slot : slots_) {
    slot.state = BufferSlot::State::FREE;
    slot.valid_size = 0;
    slot.offset = 0;
  }
}

void RecordFileReader::DrainInFlightReads() {
  if (!ring_initialized_) {
    return;
  }
  while (in_flight_reads_ > 0) {
    bool completed = false;
    if (!PollCompletions(true, &completed)) {
      break;
    }
  }
}

bool RecordFileReader::SetPosition(uint64_t target_pos) {
  if (fd_ < 0) {
    return false;
  }
  DrainInFlightReads();
  ResetBufferState();

  logical_offset_ = target_pos;
  stream_base_offset_ = target_pos;
  next_submit_offset_ = target_pos;
  end_of_file_ = false;
  reached_physical_eof_ = false;

  FillReadWindow();
  return EnsureCurrentSlot();
}

void RecordFileReader::FillReadWindow() {
  while (!reached_physical_eof_ && in_flight_reads_ < kPrefetchDepth) {
    int free_slot = -1;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
      if (slots_[i].state == BufferSlot::State::FREE) {
        free_slot = i;
        break;
      }
    }
    if (free_slot < 0) {
      break;
    }
    if (!SubmitRead(free_slot, next_submit_offset_)) {
      break;
    }
    next_submit_offset_ += kSlotSize;
  }
}

bool RecordFileReader::SubmitRead(int slot_index, uint64_t offset) {
  if (!ring_initialized_) {
    const ssize_t bytes_read =
        pread(fd_, slots_[slot_index].data, slots_[slot_index].capacity,
              static_cast<off_t>(offset));
    if (bytes_read < 0) {
      AERROR << "pread read failed, file: " << path_ << ", errno: " << errno;
      return false;
    }
    slots_[slot_index].offset = offset;
    slots_[slot_index].valid_size = static_cast<size_t>(bytes_read);
    slots_[slot_index].state =
        bytes_read == 0 ? BufferSlot::State::FREE : BufferSlot::State::READY;
    if (bytes_read == 0) {
      reached_physical_eof_ = true;
    } else {
      ready_slots_by_offset_[offset] = slot_index;
      if (static_cast<size_t>(bytes_read) < slots_[slot_index].capacity) {
        reached_physical_eof_ = true;
      }
    }
    return true;
  }
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (sqe == nullptr) {
    return false;
  }
  slots_[slot_index].offset = offset;
  slots_[slot_index].state = BufferSlot::State::IN_FLIGHT;
  if (buffers_registered_) {
    io_uring_prep_read_fixed(sqe, fd_, slots_[slot_index].data,
                             slots_[slot_index].capacity, offset, slot_index);
  } else {
    io_uring_prep_read(sqe, fd_, slots_[slot_index].data,
                       slots_[slot_index].capacity, offset);
  }
  io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(slot_index));
  if (io_uring_submit(&ring_) < 0) {
    slots_[slot_index].state = BufferSlot::State::FREE;
    AERROR << "io_uring_submit read failed, file: " << path_;
    return false;
  }
  ++in_flight_reads_;
  return true;
}

bool RecordFileReader::PollCompletions(bool wait_for_one, bool* completed) {
  *completed = false;
  struct io_uring_cqe* cqe = nullptr;
  if (wait_for_one && in_flight_reads_ > 0) {
    if (io_uring_wait_cqe(&ring_, &cqe) < 0 || cqe == nullptr) {
      AERROR << "io_uring_wait_cqe read failed, file: " << path_;
      end_of_file_ = true;
      return false;
    }
  }

  while (true) {
    if (cqe == nullptr && io_uring_peek_cqe(&ring_, &cqe) != 0) {
      break;
    }
    *completed = true;
    const int slot_index = static_cast<int>(io_uring_cqe_get_data64(cqe));
    const int res = cqe->res;
    io_uring_cqe_seen(&ring_, cqe);
    cqe = nullptr;
    --in_flight_reads_;

    if (slot_index < 0 || slot_index >= static_cast<int>(slots_.size())) {
      continue;
    }
    auto& slot = slots_[slot_index];
    if (res < 0) {
      AERROR << "io_uring read cqe error, file: " << path_ << ", res: " << res;
      slot.state = BufferSlot::State::FREE;
      end_of_file_ = true;
      return false;
    }
    if (res == 0) {
      slot.state = BufferSlot::State::FREE;
      slot.valid_size = 0;
      reached_physical_eof_ = true;
      continue;
    }
    slot.valid_size = static_cast<size_t>(res);
    slot.state = BufferSlot::State::READY;
    ready_slots_by_offset_[slot.offset] = slot_index;
    if (slot.valid_size < slot.capacity) {
      reached_physical_eof_ = true;
    }
  }
  FillReadWindow();
  return true;
}

bool RecordFileReader::EnsureCurrentSlot() {
  const uint64_t expected_slot_base =
      stream_base_offset_ +
      ((logical_offset_ - stream_base_offset_) / kSlotSize) * kSlotSize;

  if (current_slot_index_ >= 0) {
    auto& slot = slots_[current_slot_index_];
    if (slot.state == BufferSlot::State::READY && slot.offset == expected_slot_base &&
        current_slot_pos_ < slot.valid_size) {
      return true;
    }
    slot.state = BufferSlot::State::FREE;
    current_slot_index_ = -1;
    current_slot_pos_ = 0;
  }

  while (true) {
    auto it = ready_slots_by_offset_.find(expected_slot_base);
    if (it != ready_slots_by_offset_.end()) {
      current_slot_index_ = it->second;
      ready_slots_by_offset_.erase(it);
      current_slot_pos_ =
          static_cast<size_t>(logical_offset_ - slots_[current_slot_index_].offset);
      if (current_slot_pos_ >= slots_[current_slot_index_].valid_size) {
        slots_[current_slot_index_].state = BufferSlot::State::FREE;
        current_slot_index_ = -1;
        current_slot_pos_ = 0;
        end_of_file_ = true;
        return false;
      }
      return true;
    }
    if (reached_physical_eof_ && in_flight_reads_ == 0) {
      end_of_file_ = true;
      return false;
    }
    bool completed = false;
    if (!PollCompletions(false, &completed)) {
      return false;
    }
    if (!completed) {
      if (in_flight_reads_ <= 0) {
        end_of_file_ = true;
        return false;
      }
      apollo::cyber::Yield();
    }
  }
}

bool RecordFileReader::ReadBytes(void* output, size_t size) {
  char* out = reinterpret_cast<char*>(output);
  size_t copied = 0;
  while (copied < size) {
    if (!EnsureCurrentSlot()) {
      if (!end_of_file_) {
        AERROR << "Buffered data unavailable, need bytes: " << size;
      }
      return false;
    }
    auto& slot = slots_[current_slot_index_];
    const size_t available = slot.valid_size - current_slot_pos_;
    const size_t take = std::min(available, size - copied);
    memcpy(out + copied, slot.data + current_slot_pos_, take);
    current_slot_pos_ += take;
    logical_offset_ += take;
    copied += take;
    if (current_slot_pos_ >= slot.valid_size) {
      slot.state = BufferSlot::State::FREE;
      current_slot_index_ = -1;
      current_slot_pos_ = 0;
      FillReadWindow();
    }
  }
  return true;
}

bool RecordFileReader::AcquireContiguousPayload(size_t size, const char** payload) {
  if (size == 0) {
    *payload = nullptr;
    return true;
  }
  if (!EnsureCurrentSlot()) {
    return false;
  }
  auto& slot = slots_[current_slot_index_];
  if (slot.valid_size - current_slot_pos_ < size) {
    return false;
  }
  *payload = slot.data + current_slot_pos_;
  return true;
}

void RecordFileReader::AdvanceLogicalOffset(size_t size) {
  logical_offset_ += size;
  current_slot_pos_ += size;
  if (current_slot_index_ >= 0 &&
      current_slot_pos_ >= slots_[current_slot_index_].valid_size) {
    slots_[current_slot_index_].state = BufferSlot::State::FREE;
    current_slot_index_ = -1;
    current_slot_pos_ = 0;
    FillReadWindow();
  }
}

bool RecordFileReader::Reset() {
  if (!SetPosition(sizeof(Section) + HEADER_LENGTH)) {
    AERROR << "Reset position fail, file: " << path_;
    return false;
  }
  end_of_file_ = false;
  return true;
}

bool RecordFileReader::ReadHeader() {
  Section section;
  if (!ReadSection(&section)) {
    AERROR << "Read header section fail, file is broken or it is not a record "
              "file.";
    return false;
  }
  if (section.type != SectionType::SECTION_HEADER) {
    AERROR << "Check section type failed"
           << ", expect: " << SectionType::SECTION_HEADER
           << ", actual: " << section.type;
    return false;
  }
  if (!ReadSection<proto::Header>(section.size, &header_)) {
    AERROR << "Read header section fail, file is broken or it is not a record "
              "file.";
    return false;
  }
  if (!SetPosition(sizeof(Section) + HEADER_LENGTH)) {
    AERROR << "Skip bytes for reaching the nex section failed.";
    return false;
  }
  return true;
}

bool RecordFileReader::ReadIndex() {
  if (!header_.is_complete()) {
    AERROR << "Record file is not complete.";
    return false;
  }
  if (!SetPosition(header_.index_position())) {
    AERROR << "Skip bytes for reaching the index section failed.";
    return false;
  }
  Section section;
  if (!ReadSection(&section)) {
    AERROR << "Read index section fail, maybe file is broken.";
    return false;
  }
  if (section.type != SectionType::SECTION_INDEX) {
    AERROR << "Check section type failed"
           << ", expect: " << SectionType::SECTION_INDEX
           << ", actual: " << section.type;
    return false;
  }
  if (!ReadSection<proto::Index>(section.size, &index_)) {
    AERROR << "Read index section fail.";
    return false;
  }
  return Reset();
}

bool RecordFileReader::ReadSection(Section* section) {
  if (!ReadBytes(section, sizeof(Section))) {
    return false;
  }
  return true;
}

bool RecordFileReader::SkipSection(int64_t size) {
  if (size < 0) {
    AERROR << "Skip size must be non-negative, actual: " << size;
    return false;
  }
  if (size == 0) {
    return true;
  }
  const uint64_t target = logical_offset_ + static_cast<uint64_t>(size);
  if (current_slot_index_ >= 0) {
    auto& slot = slots_[current_slot_index_];
    const size_t available = slot.valid_size - current_slot_pos_;
    if (static_cast<uint64_t>(available) >= static_cast<uint64_t>(size)) {
      current_slot_pos_ += static_cast<size_t>(size);
      logical_offset_ = target;
      if (current_slot_pos_ >= slot.valid_size) {
        slot.state = BufferSlot::State::FREE;
        current_slot_index_ = -1;
        current_slot_pos_ = 0;
        FillReadWindow();
      }
      return true;
    }
  }
  return SetPosition(target);
}

}  // namespace record
}  // namespace cyber
}  // namespace apollo

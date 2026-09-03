// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0

#ifndef TESTS_PERF_TEST_BENCHMARK_SEQUENCE_TRACKER_H_
#define TESTS_PERF_TEST_BENCHMARK_SEQUENCE_TRACKER_H_

#include <algorithm>
#include <cstdint>
#include <vector>

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

enum class SequenceObservation {
  kUniqueInOrder,
  kUniqueReordered,
  kDuplicate,
  kOutOfWindow,
};

class PublisherSequenceTracker {
 public:
  explicit PublisherSequenceTracker(uint64_t sequence_capacity = 1ULL << 20)
      : sequence_capacity_(std::max<uint64_t>(1, sequence_capacity)),
        received_bitmap_(
            static_cast<size_t>((sequence_capacity_ + 63ULL) / 64ULL), 0) {}

  SequenceObservation Observe(uint64_t sequence) {
    if (sequence >= sequence_capacity_) {
      ++out_of_window_;
      return SequenceObservation::kOutOfWindow;
    }

    const size_t word = static_cast<size_t>(sequence / 64ULL);
    const uint64_t mask = 1ULL << (sequence % 64ULL);
    if ((received_bitmap_[word] & mask) != 0) {
      ++duplicates_;
      return SequenceObservation::kDuplicate;
    }

    const bool reordered = initialized_ && sequence < highest_sequence_;
    if (!initialized_) {
      initialized_ = true;
      AddObservedGap(0, sequence);
      highest_sequence_ = sequence;
    } else if (sequence > highest_sequence_) {
      AddObservedGap(highest_sequence_ + 1, sequence);
      highest_sequence_ = sequence;
    }

    received_bitmap_[word] |= mask;
    ++received_unique_;
    if (reordered) {
      ++reordered_;
      return SequenceObservation::kUniqueReordered;
    }
    return SequenceObservation::kUniqueInOrder;
  }

  bool initialized() const { return initialized_; }
  uint64_t next_sequence() const {
    return initialized_ ? highest_sequence_ + 1 : 0;
  }
  uint64_t last_sequence() const {
    return initialized_ ? highest_sequence_ : 0;
  }
  uint64_t received_unique() const { return received_unique_; }
  uint64_t gaps_observed() const { return gaps_observed_; }
  uint64_t duplicates() const { return duplicates_; }
  uint64_t reordered() const { return reordered_; }
  uint64_t out_of_window() const { return out_of_window_; }
  uint64_t sequence_capacity() const { return sequence_capacity_; }

  uint64_t UniqueForSent(uint64_t sent) const {
    const uint64_t bounded_sent = std::min(sent, sequence_capacity_);
    const size_t full_words = static_cast<size_t>(bounded_sent / 64ULL);
    uint64_t unique = 0;
    for (size_t word = 0; word < full_words; ++word) {
      unique += static_cast<uint64_t>(
          __builtin_popcountll(received_bitmap_[word]));
    }
    const uint64_t remaining = bounded_sent % 64ULL;
    if (remaining != 0) {
      const uint64_t mask = (1ULL << remaining) - 1ULL;
      unique += static_cast<uint64_t>(
          __builtin_popcountll(received_bitmap_[full_words] & mask));
    }
    return unique;
  }

  uint64_t LossForSent(uint64_t sent) const {
    return sent - UniqueForSent(sent);
  }

  uint64_t MaxConsecutiveLossForSent(uint64_t sent) const {
    uint64_t longest = 0;
    uint64_t current = 0;
    for (uint64_t sequence = 0; sequence < sent; ++sequence) {
      if (sequence >= sequence_capacity_ || !WasReceived(sequence)) {
        longest = std::max(longest, ++current);
      } else {
        current = 0;
      }
    }
    return longest;
  }

 private:
  bool WasReceived(uint64_t sequence) const {
    const size_t word = static_cast<size_t>(sequence / 64ULL);
    const uint64_t mask = 1ULL << (sequence % 64ULL);
    return (received_bitmap_[word] & mask) != 0;
  }

  void AddObservedGap(uint64_t begin, uint64_t end) {
    if (begin >= end) {
      return;
    }
    gaps_observed_ += end - begin;
  }

  uint64_t sequence_capacity_ = 0;
  std::vector<uint64_t> received_bitmap_;
  bool initialized_ = false;
  uint64_t highest_sequence_ = 0;
  uint64_t received_unique_ = 0;
  uint64_t gaps_observed_ = 0;
  uint64_t duplicates_ = 0;
  uint64_t reordered_ = 0;
  uint64_t out_of_window_ = 0;
};

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

#endif  // TESTS_PERF_TEST_BENCHMARK_SEQUENCE_TRACKER_H_

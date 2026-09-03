// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0

#include "tests/perf_test/benchmark_sequence_tracker.h"

#include "gtest/gtest.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

TEST(PublisherSequenceTrackerTest, KeepsPublisherSequencesIndependent) {
  PublisherSequenceTracker first;
  PublisherSequenceTracker second;
  first.Observe(0);
  second.Observe(0);
  first.Observe(1);
  second.Observe(1);

  EXPECT_EQ(first.LossForSent(2), 0);
  EXPECT_EQ(second.LossForSent(2), 0);
  EXPECT_EQ(first.received_unique(), 2);
  EXPECT_EQ(second.received_unique(), 2);
}

TEST(PublisherSequenceTrackerTest, ReorderedDeliveryRepairsObservedGap) {
  PublisherSequenceTracker tracker;
  tracker.Observe(0);
  tracker.Observe(2);
  EXPECT_EQ(tracker.gaps_observed(), 1);
  EXPECT_EQ(tracker.LossForSent(3), 1);

  tracker.Observe(1);
  EXPECT_EQ(tracker.reordered(), 1);
  EXPECT_EQ(tracker.duplicates(), 0);
  EXPECT_EQ(tracker.received_unique(), 3);
  EXPECT_EQ(tracker.LossForSent(3), 0);
}

TEST(PublisherSequenceTrackerTest, SeparatesDuplicatesFromRealLoss) {
  PublisherSequenceTracker tracker(5);
  tracker.Observe(1);
  tracker.Observe(1);
  tracker.Observe(3);

  EXPECT_EQ(tracker.gaps_observed(), 2);
  EXPECT_EQ(tracker.duplicates(), 1);
  EXPECT_EQ(tracker.reordered(), 0);
  EXPECT_EQ(tracker.LossForSent(5), 3);
  EXPECT_EQ(tracker.MaxConsecutiveLossForSent(5), 1);
}

TEST(PublisherSequenceTrackerTest, CountsExactUniqueReceiptWithinSentRange) {
  PublisherSequenceTracker tracker(16);
  EXPECT_EQ(tracker.Observe(4), SequenceObservation::kUniqueInOrder);
  EXPECT_EQ(tracker.Observe(1), SequenceObservation::kUniqueReordered);
  EXPECT_EQ(tracker.Observe(4), SequenceObservation::kDuplicate);
  EXPECT_EQ(tracker.Observe(7), SequenceObservation::kUniqueInOrder);
  EXPECT_EQ(tracker.Observe(3), SequenceObservation::kUniqueReordered);

  EXPECT_EQ(tracker.UniqueForSent(5), 3);
  EXPECT_EQ(tracker.LossForSent(5), 2);
  EXPECT_EQ(tracker.MaxConsecutiveLossForSent(5), 1);
  EXPECT_EQ(tracker.duplicates(), 1);
  EXPECT_EQ(tracker.reordered(), 2);
}

TEST(PublisherSequenceTrackerTest, ReportsBoundedWindowOverflow) {
  PublisherSequenceTracker tracker(4);
  EXPECT_EQ(tracker.Observe(4), SequenceObservation::kOutOfWindow);
  EXPECT_EQ(tracker.out_of_window(), 1);
  EXPECT_EQ(tracker.LossForSent(5), 5);
  EXPECT_EQ(tracker.MaxConsecutiveLossForSent(5), 5);
}

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

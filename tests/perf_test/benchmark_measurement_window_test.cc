// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "tests/perf_test/benchmark_measurement_window.h"

#include "gtest/gtest.h"
#include "tests/perf_test/benchmark_sequence_tracker.h"

namespace apollo {
namespace cyber {
namespace examples {
namespace perf_test {

TEST(MeasurementWindowMetricsTest,
     CooldownDeliveryRepairsFinalLossWithoutChangingMeasuredMetrics) {
  MeasurementWindowMetrics measured(2, 8);
  PublisherSequenceTracker first_publisher;
  PublisherSequenceTracker second_publisher;

  measured.Start();
  EXPECT_EQ(first_publisher.Observe(0), SequenceObservation::kUniqueInOrder);
  EXPECT_TRUE(measured.Record(0, 64, 100));
  EXPECT_EQ(first_publisher.Observe(2), SequenceObservation::kUniqueInOrder);
  EXPECT_TRUE(measured.Record(0, 64, 200));
  const MeasurementWindowSnapshot snapshot = measured.StopAndSnapshot();

  EXPECT_EQ(first_publisher.Observe(1), SequenceObservation::kUniqueReordered);
  EXPECT_FALSE(measured.Record(0, 64, 800));
  EXPECT_EQ(second_publisher.Observe(0), SequenceObservation::kUniqueInOrder);
  EXPECT_FALSE(measured.Record(1, 128, 900));

  EXPECT_EQ(first_publisher.LossForSent(3), 0);
  EXPECT_EQ(second_publisher.LossForSent(1), 0);
  EXPECT_EQ(snapshot.received_messages, 2);
  EXPECT_EQ(snapshot.received_bytes, 128);
  ASSERT_EQ(snapshot.latency_samples.size(), 2);
  EXPECT_EQ(snapshot.latency_samples.front(), 100);
  EXPECT_EQ(snapshot.latency_samples.back(), 200);
  EXPECT_FALSE(snapshot.DeliveryConfirmed());
}

TEST(MeasurementWindowMetricsTest, CapsOnlyMeasurementWindowLatencies) {
  MeasurementWindowMetrics measured(1, 1);
  measured.Start();
  EXPECT_TRUE(measured.Record(0, 32, 10));
  EXPECT_TRUE(measured.Record(0, 32, 20));
  const MeasurementWindowSnapshot snapshot = measured.StopAndSnapshot();

  EXPECT_EQ(snapshot.received_messages, 2);
  EXPECT_EQ(snapshot.received_bytes, 64);
  EXPECT_EQ(snapshot.dropped_samples, 1);
  ASSERT_EQ(snapshot.latency_samples.size(), 1);
  EXPECT_EQ(snapshot.latency_samples.front(), 10);
  EXPECT_TRUE(snapshot.DeliveryConfirmed());
}

}  // namespace perf_test
}  // namespace examples
}  // namespace cyber
}  // namespace apollo

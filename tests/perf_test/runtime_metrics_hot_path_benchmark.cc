// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sys/resource.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "cyber/metrics/metrics.h"

namespace {

using apollo::cyber::metrics::Clock;
using apollo::cyber::metrics::Endpoint;
using apollo::cyber::metrics::Mode;
using apollo::cyber::metrics::Registry;

bool ParseCount(const char* text, uint64_t* count) {
  if (text == nullptr || *text == '\0' || *text == '-') return false;
  char* end = nullptr;
  errno = 0;
  const auto value = std::strtoull(text, &end, 10);
  if (errno != 0 || *end != '\0' || value == 0 || value > 100000000)
    return false;
  *count = value;
  return true;
}

void Run(uint64_t iterations, const std::shared_ptr<Endpoint>& writer,
         const std::shared_ptr<Endpoint>& receiver,
         const std::shared_ptr<Endpoint>& consumer,
         std::atomic<uint64_t>* checksum) {
  for (uint64_t i = 0; i < iterations; ++i) {
    checksum->fetch_add(i + 1, std::memory_order_relaxed);
    if (writer) {
      writer->RecordPublish(true, std::chrono::microseconds(2));
      receiver->RecordReceive();
      consumer->RecordEnqueue();
      consumer->SetQueueDepth(1);
      consumer->RecordDequeue();
      consumer->SetQueueDepth(0);
      consumer->ObserveQueueLatency(std::chrono::microseconds(2));
      consumer->CallbackStart();
      consumer->ObserveCallbackLatency(std::chrono::microseconds(2));
      consumer->CallbackComplete();
    }
  }
}

uint64_t CpuNs(const rusage& usage) {
  return static_cast<uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) *
             1000000000ULL +
         static_cast<uint64_t>(usage.ru_utime.tv_usec +
                               usage.ru_stime.tv_usec) *
             1000ULL;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4 ||
      (std::strcmp(argv[1], "off") != 0 && std::strcmp(argv[1], "basic") != 0 &&
       std::strcmp(argv[1], "detailed") != 0)) {
    std::cerr << "usage: runtime_metrics_hot_path_benchmark "
                 "<off|basic|detailed> <warmup> <iterations>\n";
    return EXIT_FAILURE;
  }
  uint64_t warmup = 0;
  uint64_t iterations = 0;
  if (!ParseCount(argv[2], &warmup) || !ParseCount(argv[3], &iterations)) {
    std::cerr
        << "warmup and iterations must be positive integers <= 100000000\n";
    return EXIT_FAILURE;
  }
  const Mode mode = std::strcmp(argv[1], "off") == 0     ? Mode::Off
                    : std::strcmp(argv[1], "basic") == 0 ? Mode::Basic
                                                         : Mode::Detailed;
  auto& registry = Registry::Instance();
  registry.Configure(mode);
  auto writer = registry.RegisterWriter("/benchmark", "writer");
  auto receiver = registry.RegisterReceiver("/benchmark");
  auto consumer = registry.RegisterConsumer("/benchmark", "consumer");
  if (mode != Mode::Off && (!writer || !receiver || !consumer)) {
    std::cerr << "failed to register metric endpoints\n";
    return EXIT_FAILURE;
  }
  std::atomic<uint64_t> checksum{0};
  Run(warmup, writer, receiver, consumer, &checksum);
  rusage start_usage{};
  rusage end_usage{};
  if (getrusage(RUSAGE_SELF, &start_usage) != 0) return EXIT_FAILURE;
  const auto start = Clock::now();
  Run(iterations, writer, receiver, consumer, &checksum);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count();
  if (getrusage(RUSAGE_SELF, &end_usage) != 0) return EXIT_FAILURE;
  const auto after = registry.Snapshot();
  uint64_t published = 0;
  uint64_t received = 0;
  uint64_t enqueued = 0;
  uint64_t dequeued = 0;
  uint64_t callbacks = 0;
  uint64_t publish_samples = 0;
  uint64_t queue_samples = 0;
  uint64_t callback_samples = 0;
  for (const auto& endpoint : after.endpoints) {
    switch (endpoint.kind) {
      case apollo::cyber::metrics::Kind::Writer:
        published = endpoint.publish_count;
        publish_samples = endpoint.publish_latency.count;
        break;
      case apollo::cyber::metrics::Kind::Receiver:
        received = endpoint.receive_count;
        break;
      case apollo::cyber::metrics::Kind::Consumer:
        enqueued = endpoint.enqueue_count;
        dequeued = endpoint.dequeue_count;
        callbacks = endpoint.callback_completed_count;
        queue_samples = endpoint.queue_latency.count;
        callback_samples = endpoint.callback_latency.count;
        break;
      case apollo::cyber::metrics::Kind::Task:
        break;
    }
  }
  const auto expected = mode == Mode::Off ? 0 : iterations + warmup;
  if (published != expected || received != expected || enqueued != expected ||
      dequeued != expected || callbacks != expected) {
    std::cerr << "metrics counts do not match measured work\n";
    return EXIT_FAILURE;
  }
  std::cout << "{\"mode\":\"" << argv[1] << "\",\"warmup\":" << warmup
            << ",\"iterations\":" << iterations << ",\"elapsed_ns\":" << elapsed
            << ",\"cpu_ns\":" << CpuNs(end_usage) - CpuNs(start_usage)
            << ",\"ns_per_iteration\":"
            << static_cast<double>(elapsed) / iterations
            << ",\"max_rss_kb\":" << end_usage.ru_maxrss
            << ",\"live_series\":" << after.live_series
            << ",\"checksum\":" << checksum.load(std::memory_order_relaxed)
            << ",\"publish_count\":" << published
            << ",\"receive_count\":" << received
            << ",\"enqueue_count\":" << enqueued
            << ",\"dequeue_count\":" << dequeued
            << ",\"callback_count\":" << callbacks
            << ",\"publish_window_samples\":" << publish_samples
            << ",\"queue_window_samples\":" << queue_samples
            << ",\"callback_window_samples\":" << callback_samples << "}\n";
  return EXIT_SUCCESS;
}

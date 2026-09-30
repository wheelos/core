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

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cyber/metrics/metrics.h"

namespace {

using apollo::cyber::metrics::Clock;
using apollo::cyber::metrics::Endpoint;
using apollo::cyber::metrics::Mode;
using apollo::cyber::metrics::Registry;

bool ParseCount(const char* text, size_t upper, size_t* value) {
  const std::string input(text);
  if (input.empty()) return false;
  const auto result = std::from_chars(input.data(), input.data() + input.size(),
                                      *value);
  return result.ec == std::errc{} && result.ptr == input.data() + input.size() &&
         *value > 0 && *value <= upper;
}

uint64_t CpuNs(const rusage& usage) {
  return static_cast<uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) *
             1000000000ULL +
         static_cast<uint64_t>(usage.ru_utime.tv_usec +
                               usage.ru_stime.tv_usec) *
             1000ULL;
}

uint64_t Ns(Clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

uint64_t Percentile(std::vector<uint64_t>* samples, size_t numerator) {
  std::sort(samples->begin(), samples->end());
  return (*samples)[(samples->size() * numerator + 99) / 100 - 1];
}

}  // namespace

int main(int argc, char** argv) {
  size_t series = 0;
  size_t iterations = 0;
  if (argc != 3 || !ParseCount(argv[1], Registry::kMaxSeries, &series) ||
      !ParseCount(argv[2], 1000, &iterations)) {
    std::cerr << "usage: runtime_metrics_snapshot_benchmark "
                 "<series:1..4096> <iterations:1..1000>\n";
    return EXIT_FAILURE;
  }

  auto& registry = Registry::Instance();
  registry.Configure(Mode::Basic);
  std::vector<std::shared_ptr<Endpoint>> consumers;
  consumers.reserve(series);
  for (size_t i = 0; i < series; ++i) {
    auto endpoint =
        registry.RegisterConsumer("/snapshot", "reader_" + std::to_string(i));
    if (!endpoint) {
      std::cerr << "failed to register metric endpoint\n";
      return EXIT_FAILURE;
    }
    endpoint->RecordEnqueue();
    endpoint->SetQueueDepth(1);
    endpoint->ObserveQueueLatency(std::chrono::microseconds(2));
    endpoint->ObserveCallbackLatency(std::chrono::microseconds(4));
    consumers.push_back(std::move(endpoint));
  }

  if (registry.Snapshot().live_series != series) {
    std::cerr << "metric series count mismatch\n";
    return EXIT_FAILURE;
  }
  std::vector<uint64_t> snapshot_ns;
  std::vector<uint64_t> json_ns;
  snapshot_ns.reserve(iterations);
  json_ns.reserve(iterations);
  rusage before{}, after{};
  if (getrusage(RUSAGE_SELF, &before) != 0) return EXIT_FAILURE;
  size_t bytes = 0;
  for (size_t i = 0; i < iterations; ++i) {
    const auto start = Clock::now();
    const auto snapshot = registry.Snapshot();
    const auto prepared = Clock::now();
    const auto json = snapshot.ToJson();
    const auto end = Clock::now();
    if (snapshot.live_series != series || json.empty()) {
      std::cerr << "snapshot is incomplete\n";
      return EXIT_FAILURE;
    }
    bytes = json.size();
    snapshot_ns.push_back(Ns(prepared - start));
    json_ns.push_back(Ns(end - prepared));
  }
  if (getrusage(RUSAGE_SELF, &after) != 0) return EXIT_FAILURE;

  std::cout << "{\"series\":" << series << ",\"iterations\":" << iterations
            << ",\"snapshot_p50_ns\":" << Percentile(&snapshot_ns, 50)
            << ",\"snapshot_p99_ns\":" << Percentile(&snapshot_ns, 99)
            << ",\"json_p50_ns\":" << Percentile(&json_ns, 50)
            << ",\"json_p99_ns\":" << Percentile(&json_ns, 99)
            << ",\"cpu_ns\":" << CpuNs(after) - CpuNs(before)
            << ",\"json_bytes\":" << bytes
            << ",\"max_rss_kb\":" << after.ru_maxrss << "}\n";
  return EXIT_SUCCESS;
}

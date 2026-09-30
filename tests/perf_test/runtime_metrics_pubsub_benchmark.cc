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
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cyber/proto/unit_test.pb.h"

#include "cyber/cyber.h"
#include "cyber/metrics/metrics.h"

namespace {

using apollo::cyber::metrics::Clock;
using apollo::cyber::metrics::Kind;
using apollo::cyber::metrics::Mode;
using apollo::cyber::metrics::Registry;
using namespace std::chrono_literals;

uint64_t CpuNs(const rusage& usage) {
  return static_cast<uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) *
             1000000000ULL +
         static_cast<uint64_t>(usage.ru_utime.tv_usec +
                               usage.ru_stime.tv_usec) *
             1000ULL;
}

uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

bool ParseCount(const char* text, uint64_t* value, uint64_t limit) {
  if (!text || !*text || *text == '-') return false;
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoull(text, &end, 10);
  if (errno || *end || parsed == 0 || parsed > limit) return false;
  *value = parsed;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6 ||
      (std::string(argv[1]) != "off" && std::string(argv[1]) != "basic" &&
       std::string(argv[1]) != "detailed")) {
    std::cerr << "usage: runtime_metrics_pubsub_benchmark "
                 "<off|basic|detailed> <warmup> <messages> <payload_bytes> "
                 "<rate_hz>\n";
    return EXIT_FAILURE;
  }
  uint64_t warmup = 0, messages = 0, payload_bytes = 0, rate_hz = 0;
  if (!ParseCount(argv[2], &warmup, 100000) ||
      !ParseCount(argv[3], &messages, 100000) ||
      !ParseCount(argv[4], &payload_bytes, 1048576) ||
      !ParseCount(argv[5], &rate_hz, 100000)) {
    std::cerr << "invalid warmup, messages, payload_bytes or rate_hz\n";
    return EXIT_FAILURE;
  }
  if (!apollo::cyber::Init(argv[0])) {
    std::cerr << "cyber Init failed\n";
    return EXIT_FAILURE;
  }
  auto& registry = Registry::Instance();
  const Mode mode = std::string(argv[1]) == "off"     ? Mode::Off
                    : std::string(argv[1]) == "basic" ? Mode::Basic
                                                      : Mode::Detailed;
  registry.Configure(mode);
  const std::string channel =
      "/runtime_metrics_benchmark_" + std::to_string(getpid());
  auto sub = apollo::cyber::CreateNode("metrics_benchmark_sub_" +
                                       std::to_string(getpid()));
  auto pub = apollo::cyber::CreateNode("metrics_benchmark_pub_" +
                                       std::to_string(getpid()));
  if (!sub || !pub) {
    std::cerr << "failed to create benchmark nodes\n";
    return EXIT_FAILURE;
  }
  std::mutex mutex;
  std::condition_variable cv;
  uint64_t warmed = 0;
  std::vector<uint64_t> latencies;
  latencies.reserve(messages);
  bool recording = true;
  apollo::cyber::ReaderConfig reader_config;
  reader_config.channel_name = channel;
  reader_config.pending_queue_size = 1024;
  auto reader = sub->CreateReader<apollo::cyber::proto::UnitTest>(
      reader_config,
      [&](const std::shared_ptr<apollo::cyber::proto::UnitTest>& msg) {
        const auto now = NowNs();
        std::lock_guard<std::mutex> lock(mutex);
        if (msg->case_name() == "warmup") {
          ++warmed;
        } else if (recording) {
          const auto sent = std::stoull(msg->case_name());
          latencies.push_back(now - sent);
        }
        cv.notify_one();
      });
  auto writer = pub->CreateWriter<apollo::cyber::proto::UnitTest>(channel);
  if (!reader || !writer) {
    std::cerr << "failed to create benchmark reader/writer\n";
    return EXIT_FAILURE;
  }
  const auto deadline = Clock::now() + 8s;
  while (!writer->HasReader() && Clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  if (!writer->HasReader()) {
    std::cerr << "reader discovery timed out\n";
    return EXIT_FAILURE;
  }
  auto payload = std::make_shared<apollo::cyber::proto::UnitTest>();
  payload->set_class_name(std::string(payload_bytes, 'x'));
  const auto period = std::chrono::nanoseconds(1000000000ULL / rate_hz);
  auto next = Clock::now();
  payload->set_case_name("warmup");
  for (uint64_t i = 0; i < warmup; ++i) {
    if (!writer->Write(payload)) {
      std::cerr << "warmup write failed\n";
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_until(next += period);
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_until(lock, Clock::now() + 5s,
                       [&] { return warmed == warmup; })) {
      std::cerr << "warmup delivery incomplete: " << warmed << "/" << warmup
                << "\n";
      return EXIT_FAILURE;
    }
  }
  rusage before{}, after{};
  if (getrusage(RUSAGE_SELF, &before) != 0) {
    std::cerr << "getrusage before measurement failed\n";
    return EXIT_FAILURE;
  }
  const auto start = Clock::now();
  next = start;
  for (uint64_t i = 0; i < messages; ++i) {
    auto message = std::make_shared<apollo::cyber::proto::UnitTest>(*payload);
    message->set_case_name(std::to_string(NowNs()));
    if (!writer->Write(message)) {
      std::cerr << "measured write failed\n";
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_until(next += period);
  }
  const auto send_end = Clock::now();
  std::vector<uint64_t> samples;
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_until(lock, Clock::now() + 2s,
                  [&] { return latencies.size() == messages; });
    recording = false;
    samples = std::move(latencies);
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count();
  const auto send_elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(send_end - start)
          .count();
  if (getrusage(RUSAGE_SELF, &after) != 0) {
    std::cerr << "getrusage after measurement failed\n";
    return EXIT_FAILURE;
  }
  const auto snapshot = registry.Snapshot();
  uint64_t published = 0, received = 0, callbacks = 0;
  for (const auto& endpoint : snapshot.endpoints) {
    if (endpoint.channel != channel) continue;
    if (endpoint.kind == Kind::Writer) published += endpoint.publish_count;
    if (endpoint.kind == Kind::Receiver) received += endpoint.receive_count;
    if (endpoint.kind == Kind::Consumer)
      callbacks += endpoint.callback_completed_count;
  }
  if (mode == Mode::Off ? (published || received || callbacks)
                        : (published != warmup + messages || received == 0 ||
                           callbacks == 0)) {
    std::cerr << "metric endpoints did not record the selected mode\n";
    return EXIT_FAILURE;
  }
  std::sort(samples.begin(), samples.end());
  if (samples.empty()) {
    std::cerr << "no measured messages delivered\n";
    return EXIT_FAILURE;
  }
  const auto p99 = samples[(samples.size() * 99 + 99) / 100 - 1];
  std::cout << "{\"mode\":\"" << argv[1] << "\",\"warmup\":" << warmup
            << ",\"messages\":" << messages
            << ",\"payload_bytes\":" << payload_bytes
            << ",\"rate_hz\":" << rate_hz << ",\"received\":" << samples.size()
            << ",\"elapsed_ns\":" << elapsed
            << ",\"send_elapsed_ns\":" << send_elapsed
            << ",\"drain_elapsed_ns\":" << elapsed - send_elapsed
            << ",\"cpu_ns\":" << CpuNs(after) - CpuNs(before)
            << ",\"p99_ns\":" << p99 << ",\"max_rss_kb\":" << after.ru_maxrss
            << ",\"live_series\":" << snapshot.live_series
            << ",\"publish_count\":" << published
            << ",\"receive_count\":" << received
            << ",\"callback_count\":" << callbacks << "}\n";
  reader.reset();
  writer.reset();
  sub.reset();
  pub.reset();
  apollo::cyber::Clear();
  return EXIT_SUCCESS;
}

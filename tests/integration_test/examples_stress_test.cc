// Copyright 2025 WheelOS. All Rights Reserved.
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

//  Created Date: 2026-05-31
//  Author: daohu527

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "gtest/gtest.h"

#include "examples/proto/examples.pb.h"

#include "tests/integration_test/examples_test_utils.h"

namespace apollo {
namespace cyber {
namespace examples {

namespace {

using apollo::cyber::examples::proto::Chatter;
using apollo::cyber::examples::proto::Driver;
using apollo::cyber::examples::test::kWarmupSeqBase;
using apollo::cyber::examples::test::MakePayload;
using apollo::cyber::examples::test::MakeReaderConfig;
using apollo::cyber::examples::test::UniqueName;
using apollo::cyber::examples::test::WaitFor;

struct PubSubLoadCase {
  const char* name;
  size_t writer_count;
  size_t reader_count;
  size_t payload_size;
  size_t messages_per_writer;
  double min_throughput;
};

struct ServiceLoadCase {
  const char* name;
  size_t client_count;
  size_t requests_per_client;
  size_t payload_size;
  double min_ops_per_second;
};

struct ReaderState {
  std::unordered_set<size_t> warmup_writers;
  std::unordered_set<uint64_t> received_keys;
  size_t duplicate_count = 0;
  size_t payload_mismatch = 0;
  std::chrono::steady_clock::time_point last_receive;
};

void RunPubSubLoadCase(const std::string& channel_name,
                       const PubSubLoadCase& load_case) {
  const size_t total_messages =
      load_case.writer_count * load_case.messages_per_writer;
  const uint32_t queue_depth =
      static_cast<uint32_t>(total_messages * 2 + load_case.writer_count + 8);
  const auto timeout = std::chrono::seconds(20);

  std::mutex mutex;
  std::vector<ReaderState> reader_states(load_case.reader_count);
  std::vector<std::string> expected_payloads;
  expected_payloads.reserve(load_case.writer_count);
  for (size_t writer_index = 0; writer_index < load_case.writer_count;
       ++writer_index) {
    expected_payloads.emplace_back(MakePayload(
        load_case.payload_size, static_cast<uint8_t>(writer_index + 1)));
  }

  std::vector<std::shared_ptr<apollo::cyber::Node>> reader_nodes;
  std::vector<std::shared_ptr<apollo::cyber::Reader<Chatter>>> readers;
  reader_nodes.reserve(load_case.reader_count);
  readers.reserve(load_case.reader_count);
  for (size_t reader_index = 0; reader_index < load_case.reader_count;
       ++reader_index) {
    reader_nodes.emplace_back(apollo::cyber::CreateNode(
        UniqueName("stress_listener_" + std::string(load_case.name) + "_" +
                   std::to_string(reader_index))));
    auto reader = reader_nodes.back()->CreateReader<Chatter>(
        MakeReaderConfig(channel_name, queue_depth),
        [&, reader_index](const std::shared_ptr<Chatter>& msg) {
          const auto writer_index = static_cast<size_t>(msg->timestamp());
          std::lock_guard<std::mutex> lock(mutex);
          if (msg->seq() >= kWarmupSeqBase) {
            reader_states[reader_index].warmup_writers.insert(writer_index);
            return;
          }
          if (writer_index >= expected_payloads.size() ||
              msg->content() != expected_payloads[writer_index]) {
            ++reader_states[reader_index].payload_mismatch;
          }
          const uint64_t message_key =
              (static_cast<uint64_t>(writer_index) << 32) | msg->seq();
          if (!reader_states[reader_index]
                   .received_keys.insert(message_key)
                   .second) {
            ++reader_states[reader_index].duplicate_count;
            return;
          }
          reader_states[reader_index].last_receive =
              std::chrono::steady_clock::now();
        });
    ASSERT_NE(reader, nullptr);
    readers.emplace_back(std::move(reader));
  }

  std::vector<std::shared_ptr<apollo::cyber::Node>> writer_nodes;
  std::vector<std::shared_ptr<apollo::cyber::Writer<Chatter>>> writers;
  writer_nodes.reserve(load_case.writer_count);
  writers.reserve(load_case.writer_count);
  for (size_t writer_index = 0; writer_index < load_case.writer_count;
       ++writer_index) {
    writer_nodes.emplace_back(apollo::cyber::CreateNode(
        UniqueName("stress_talker_" + std::string(load_case.name) + "_" +
                   std::to_string(writer_index))));
    auto writer = writer_nodes.back()->CreateWriter<Chatter>(channel_name);
    ASSERT_NE(writer, nullptr);
    writers.emplace_back(std::move(writer));
  }

  for (size_t attempt = 0; attempt < 40; ++attempt) {
    if (WaitFor(
            [&]() {
              std::lock_guard<std::mutex> lock(mutex);
              return std::all_of(reader_states.begin(), reader_states.end(),
                                 [&](const ReaderState& state) {
                                   return state.warmup_writers.size() ==
                                          load_case.writer_count;
                                 });
            },
            std::chrono::milliseconds(120))) {
      break;
    }
    for (size_t writer_index = 0; writer_index < load_case.writer_count;
         ++writer_index) {
      auto warmup = std::make_shared<Chatter>();
      warmup->set_seq(kWarmupSeqBase + writer_index);
      warmup->set_content("warmup_" + std::to_string(writer_index));
      warmup->set_timestamp(writer_index);
      ASSERT_TRUE(writers[writer_index]->Write(warmup));
    }
  }

  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::all_of(reader_states.begin(), reader_states.end(),
                           [&](const ReaderState& state) {
                             return state.warmup_writers.size() ==
                                    load_case.writer_count;
                           });
      },
      std::chrono::seconds(5)));

  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> senders;
  std::atomic<size_t> write_failures{0};
  senders.reserve(load_case.writer_count);
  for (size_t writer_index = 0; writer_index < load_case.writer_count;
       ++writer_index) {
    senders.emplace_back([&, writer_index]() {
      for (size_t seq = 0; seq < load_case.messages_per_writer; ++seq) {
        auto msg = std::make_shared<Chatter>();
        msg->set_seq(seq);
        msg->set_content(expected_payloads[writer_index]);
        msg->set_timestamp(writer_index);
        msg->set_lidar_timestamp(seq);
        if (!writers[writer_index]->Write(msg)) {
          ++write_failures;
        }
        if ((seq + 1) % 16 == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    });
  }
  for (auto& sender : senders) {
    sender.join();
  }
  EXPECT_EQ(write_failures.load(), 0U);

  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::all_of(reader_states.begin(), reader_states.end(),
                           [&](const ReaderState& state) {
                             return state.received_keys.size() ==
                                    total_messages;
                           });
      },
      timeout));

  auto last_receive = start;
  size_t duplicate_count = 0;
  size_t payload_mismatch = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& state : reader_states) {
      duplicate_count += state.duplicate_count;
      payload_mismatch += state.payload_mismatch;
      last_receive = std::max(last_receive, state.last_receive);
    }
  }

  EXPECT_EQ(duplicate_count, 0U);
  EXPECT_EQ(payload_mismatch, 0U);

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      last_receive - start);
  const double throughput =
      (1000.0 * static_cast<double>(total_messages)) /
      static_cast<double>(std::max<int64_t>(1, elapsed.count()));
  EXPECT_LT(elapsed.count(), timeout.count() * 1000);
  EXPECT_GT(throughput, load_case.min_throughput);
}

void RunServiceLoadCase(const std::string& service_name,
                        const ServiceLoadCase& load_case) {
  auto server_node = apollo::cyber::CreateNode(
      UniqueName("stress_service_" + std::string(load_case.name)));
  auto server = server_node->CreateService<Driver, Driver>(
      service_name, [](const std::shared_ptr<Driver>& request,
                       std::shared_ptr<Driver>& response) {
        response->set_content(request->content());
        response->set_msg_id(request->msg_id() + 1000);
        response->set_timestamp(request->timestamp() + 10);
      });
  ASSERT_NE(server, nullptr);

  std::vector<std::shared_ptr<apollo::cyber::Node>> client_nodes;
  std::vector<std::shared_ptr<apollo::cyber::Client<Driver, Driver>>> clients;
  client_nodes.reserve(load_case.client_count);
  clients.reserve(load_case.client_count);
  for (size_t client_index = 0; client_index < load_case.client_count;
       ++client_index) {
    client_nodes.emplace_back(apollo::cyber::CreateNode(
        UniqueName("stress_client_" + std::string(load_case.name) + "_" +
                   std::to_string(client_index))));
    auto client =
        client_nodes.back()->CreateClient<Driver, Driver>(service_name);
    ASSERT_NE(client, nullptr);
    clients.emplace_back(std::move(client));
  }

  for (size_t client_index = 0; client_index < load_case.client_count;
       ++client_index) {
    auto warmup = std::make_shared<Driver>();
    warmup->set_content(
        MakePayload(32, static_cast<uint8_t>(client_index + 1)));
    warmup->set_msg_id(client_index);
    warmup->set_timestamp(client_index);
    std::shared_ptr<Driver> response;
    ASSERT_TRUE(WaitFor(
        [&]() {
          response = clients[client_index]->SendRequest(warmup);
          return response != nullptr;
        },
        std::chrono::seconds(3)));
    ASSERT_NE(response, nullptr);
  }

  std::atomic<size_t> failures{0};
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(load_case.client_count);
  for (size_t client_index = 0; client_index < load_case.client_count;
       ++client_index) {
    workers.emplace_back([&, client_index]() {
      for (size_t request_index = 0;
           request_index < load_case.requests_per_client; ++request_index) {
        auto request = std::make_shared<Driver>();
        request->set_content(MakePayload(
            load_case.payload_size,
            static_cast<uint8_t>(client_index * 10 + request_index + 1)));
        request->set_msg_id(client_index * 1000 + request_index);
        request->set_timestamp(client_index * 10000 + request_index);
        auto response = clients[client_index]->SendRequest(request);
        if (response == nullptr || response->content() != request->content() ||
            response->msg_id() != request->msg_id() + 1000 ||
            response->timestamp() != request->timestamp() + 10) {
          ++failures;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  const double ops_per_second =
      (1000.0 * static_cast<double>(load_case.client_count *
                                    load_case.requests_per_client)) /
      static_cast<double>(std::max<int64_t>(1, elapsed.count()));
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_LT(elapsed.count(), 10000);
  EXPECT_GT(ops_per_second, load_case.min_ops_per_second);
}

class ExamplesStressTest : public ::testing::Test {
 protected:
  void SetUp() override {
    channel_name_ = UniqueName("stress_channel");
    service_name_ = UniqueName("stress_service");
  }

  std::string channel_name_;
  std::string service_name_;
};

TEST_F(ExamplesStressTest, SingleWriterSingleReaderPayloadMatrix) {
  const std::vector<PubSubLoadCase> load_cases = {
      {"small_payload", 1, 1, 32, 256, 40.0},
      {"medium_payload", 1, 1, 4096, 128, 15.0},
      {"large_payload", 1, 1, 65536, 32, 4.0},
  };

  for (const auto& load_case : load_cases) {
    SCOPED_TRACE(load_case.name);
    RunPubSubLoadCase(
        UniqueName("stress_channel_" + std::string(load_case.name)), load_case);
  }
}

TEST_F(ExamplesStressTest, SingleWriterMultipleReadersFanoutLoad) {
  RunPubSubLoadCase(UniqueName("stress_channel_fanout"),
                    {"fanout", 1, 3, 1024, 96, 12.0});
}

TEST_F(ExamplesStressTest, MultipleWritersSingleReaderFanInLoad) {
  RunPubSubLoadCase(UniqueName("stress_channel_fanin"),
                    {"fanin", 3, 1, 1024, 64, 12.0});
}

TEST_F(ExamplesStressTest, ServiceBurstMatrix) {
  const std::vector<ServiceLoadCase> load_cases = {
      {"service_small", 3, 24, 64, 10.0},
      {"service_large", 3, 12, 8192, 5.0},
  };

  for (const auto& load_case : load_cases) {
    SCOPED_TRACE(load_case.name);
    RunServiceLoadCase(
        UniqueName("stress_service_" + std::string(load_case.name)), load_case);
  }
}

}  // namespace

}  // namespace examples
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  const auto cyber_path = (std::filesystem::current_path() / "cyber").string();
  setenv("CYBER_PATH", cyber_path.c_str(), 1);
  testing::InitGoogleTest(&argc, argv);
  apollo::cyber::Init(argv[0]);
  const int result = RUN_ALL_TESTS();
  apollo::cyber::Clear();
  return result;
}

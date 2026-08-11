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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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

class ExamplesIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    channel_name_ = UniqueName("channel_chatter");
    service_name_ = UniqueName("test_server");
  }

  std::string channel_name_;
  std::string service_name_;
};

TEST_F(ExamplesIntegrationTest, TalkerListenerRoundTrip) {
  std::mutex mutex;
  std::condition_variable cv;
  bool received = false;
  Chatter received_msg;

  auto listener_node = apollo::cyber::CreateNode(UniqueName("listener"));
  auto reader = listener_node->CreateReader<Chatter>(
      channel_name_, [&](const std::shared_ptr<Chatter>& msg) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          received_msg = *msg;
          received = true;
        }
        cv.notify_one();
      });
  ASSERT_NE(reader, nullptr);

  auto talker_node = apollo::cyber::CreateNode(UniqueName("talker"));
  auto writer = talker_node->CreateWriter<Chatter>(channel_name_);
  ASSERT_NE(writer, nullptr);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  auto msg = std::make_shared<Chatter>();
  msg->set_seq(7);
  msg->set_content("Hello, apollo!");
  msg->set_timestamp(123);
  msg->set_lidar_timestamp(456);

  ASSERT_TRUE(writer->Write(msg));

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3),
                          [&received]() { return received; }));
  EXPECT_EQ(received_msg.seq(), 7U);
  EXPECT_EQ(received_msg.content(), "Hello, apollo!");
  EXPECT_EQ(received_msg.timestamp(), 123U);
  EXPECT_EQ(received_msg.lidar_timestamp(), 456U);
}

TEST_F(ExamplesIntegrationTest, BinaryPayloadMatrixPreservesContent) {
  const std::vector<std::string> payloads = {
      MakePayload(0, 1),    MakePayload(1, 2),    MakePayload(64, 3),
      MakePayload(1024, 4), MakePayload(8192, 5), MakePayload(65536, 6)};

  std::mutex mutex;
  bool warmup_received = false;
  std::vector<Chatter> received_messages;

  auto listener_node = apollo::cyber::CreateNode(UniqueName("binary_listener"));
  auto reader = listener_node->CreateReader<Chatter>(
      MakeReaderConfig(channel_name_, payloads.size() * 2 + 8),
      [&](const std::shared_ptr<Chatter>& msg) {
        std::lock_guard<std::mutex> lock(mutex);
        if (msg->seq() == kWarmupSeqBase) {
          warmup_received = true;
          return;
        }
        received_messages.push_back(*msg);
      });
  ASSERT_NE(reader, nullptr);

  auto talker_node = apollo::cyber::CreateNode(UniqueName("binary_talker"));
  auto writer = talker_node->CreateWriter<Chatter>(channel_name_);
  ASSERT_NE(writer, nullptr);

  for (size_t attempt = 0; attempt < 30; ++attempt) {
    if (WaitFor(
            [&]() {
              std::lock_guard<std::mutex> lock(mutex);
              return warmup_received;
            },
            std::chrono::milliseconds(100))) {
      break;
    }
    auto warmup = std::make_shared<Chatter>();
    warmup->set_seq(kWarmupSeqBase);
    warmup->set_content("warmup");
    ASSERT_TRUE(writer->Write(warmup));
  }
  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return warmup_received;
      },
      std::chrono::seconds(1)));

  for (size_t i = 0; i < payloads.size(); ++i) {
    auto msg = std::make_shared<Chatter>();
    msg->set_seq(i);
    msg->set_content(payloads[i]);
    msg->set_timestamp(i + 100);
    msg->set_lidar_timestamp(i + 200);
    ASSERT_TRUE(writer->Write(msg));
  }

  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return received_messages.size() == payloads.size();
      },
      std::chrono::seconds(5)));

  std::lock_guard<std::mutex> lock(mutex);
  ASSERT_EQ(received_messages.size(), payloads.size());
  for (size_t i = 0; i < payloads.size(); ++i) {
    EXPECT_EQ(received_messages[i].seq(), i);
    EXPECT_EQ(received_messages[i].content(), payloads[i]);
    EXPECT_EQ(received_messages[i].timestamp(), i + 100);
    EXPECT_EQ(received_messages[i].lidar_timestamp(), i + 200);
  }
}

TEST_F(ExamplesIntegrationTest, SingleWriterMultipleReadersFanout) {
  constexpr size_t kReaderCount = 3;
  constexpr size_t kMessageCount = 12;
  const std::vector<std::string> payloads = {
      MakePayload(128, 11),  MakePayload(256, 12),  MakePayload(384, 13),
      MakePayload(512, 14),  MakePayload(640, 15),  MakePayload(768, 16),
      MakePayload(896, 17),  MakePayload(1024, 18), MakePayload(1152, 19),
      MakePayload(1280, 20), MakePayload(1408, 21), MakePayload(1536, 22)};

  struct ReaderState {
    bool warmup_received = false;
    std::vector<Chatter> messages;
  };

  std::mutex mutex;
  std::vector<ReaderState> reader_states(kReaderCount);
  std::vector<std::shared_ptr<apollo::cyber::Node>> reader_nodes;
  std::vector<std::shared_ptr<apollo::cyber::Reader<Chatter>>> readers;
  reader_nodes.reserve(kReaderCount);
  readers.reserve(kReaderCount);

  for (size_t reader_index = 0; reader_index < kReaderCount; ++reader_index) {
    reader_nodes.emplace_back(apollo::cyber::CreateNode(
        UniqueName("fanout_listener_" + std::to_string(reader_index))));
    auto reader = reader_nodes.back()->CreateReader<Chatter>(
        MakeReaderConfig(channel_name_, kMessageCount * 2),
        [&, reader_index](const std::shared_ptr<Chatter>& msg) {
          std::lock_guard<std::mutex> lock(mutex);
          if (msg->seq() == kWarmupSeqBase) {
            reader_states[reader_index].warmup_received = true;
            return;
          }
          reader_states[reader_index].messages.push_back(*msg);
        });
    ASSERT_NE(reader, nullptr);
    readers.emplace_back(std::move(reader));
  }

  auto talker_node = apollo::cyber::CreateNode(UniqueName("fanout_talker"));
  auto writer = talker_node->CreateWriter<Chatter>(channel_name_);
  ASSERT_NE(writer, nullptr);

  for (size_t attempt = 0; attempt < 30; ++attempt) {
    if (WaitFor(
            [&]() {
              std::lock_guard<std::mutex> lock(mutex);
              for (const auto& state : reader_states) {
                if (!state.warmup_received) {
                  return false;
                }
              }
              return true;
            },
            std::chrono::milliseconds(100))) {
      break;
    }
    auto warmup = std::make_shared<Chatter>();
    warmup->set_seq(kWarmupSeqBase);
    warmup->set_content("fanout_warmup");
    ASSERT_TRUE(writer->Write(warmup));
  }

  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& state : reader_states) {
          if (!state.warmup_received) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(3)));

  for (size_t i = 0; i < kMessageCount; ++i) {
    auto msg = std::make_shared<Chatter>();
    msg->set_seq(i);
    msg->set_content(payloads[i]);
    msg->set_timestamp(i + 300);
    msg->set_lidar_timestamp(i + 400);
    ASSERT_TRUE(writer->Write(msg));
  }

  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& state : reader_states) {
          if (state.messages.size() != kMessageCount) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(5)));

  std::lock_guard<std::mutex> lock(mutex);
  for (size_t reader_index = 0; reader_index < kReaderCount; ++reader_index) {
    SCOPED_TRACE("reader_" + std::to_string(reader_index));
    ASSERT_EQ(reader_states[reader_index].messages.size(), kMessageCount);
    for (size_t i = 0; i < kMessageCount; ++i) {
      EXPECT_EQ(reader_states[reader_index].messages[i].seq(), i);
      EXPECT_EQ(reader_states[reader_index].messages[i].content(), payloads[i]);
      EXPECT_EQ(reader_states[reader_index].messages[i].timestamp(), i + 300);
      EXPECT_EQ(reader_states[reader_index].messages[i].lidar_timestamp(),
                i + 400);
    }
  }
}

TEST_F(ExamplesIntegrationTest, MultipleWritersSingleReaderFanIn) {
  constexpr size_t kWriterCount = 3;
  constexpr size_t kMessagesPerWriter = 10;
  const std::vector<std::string> payloads = {
      MakePayload(256, 31), MakePayload(512, 32), MakePayload(768, 33)};

  std::mutex mutex;
  std::set<size_t> warmup_writers;
  std::vector<std::vector<Chatter>> messages_by_writer(kWriterCount);

  auto listener_node = apollo::cyber::CreateNode(UniqueName("fanin_listener"));
  auto reader = listener_node->CreateReader<Chatter>(
      MakeReaderConfig(channel_name_, kWriterCount * kMessagesPerWriter * 2),
      [&](const std::shared_ptr<Chatter>& msg) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto writer_index = static_cast<size_t>(msg->timestamp());
        if (msg->seq() >= kWarmupSeqBase) {
          warmup_writers.insert(writer_index);
          return;
        }
        if (writer_index < messages_by_writer.size()) {
          messages_by_writer[writer_index].push_back(*msg);
        }
      });
  ASSERT_NE(reader, nullptr);

  std::vector<std::shared_ptr<apollo::cyber::Node>> writer_nodes;
  std::vector<std::shared_ptr<apollo::cyber::Writer<Chatter>>> writers;
  writer_nodes.reserve(kWriterCount);
  writers.reserve(kWriterCount);
  for (size_t writer_index = 0; writer_index < kWriterCount; ++writer_index) {
    writer_nodes.emplace_back(apollo::cyber::CreateNode(
        UniqueName("fanin_talker_" + std::to_string(writer_index))));
    auto writer = writer_nodes.back()->CreateWriter<Chatter>(channel_name_);
    ASSERT_NE(writer, nullptr);
    writers.emplace_back(std::move(writer));
  }

  for (size_t attempt = 0; attempt < 30; ++attempt) {
    if (WaitFor(
            [&]() {
              std::lock_guard<std::mutex> lock(mutex);
              return warmup_writers.size() == kWriterCount;
            },
            std::chrono::milliseconds(100))) {
      break;
    }
    for (size_t writer_index = 0; writer_index < kWriterCount; ++writer_index) {
      auto warmup = std::make_shared<Chatter>();
      warmup->set_seq(kWarmupSeqBase + writer_index);
      warmup->set_content("fanin_warmup_" + std::to_string(writer_index));
      warmup->set_timestamp(writer_index);
      ASSERT_TRUE(writers[writer_index]->Write(warmup));
    }
  }

  ASSERT_TRUE(WaitFor(
      [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return warmup_writers.size() == kWriterCount;
      },
      std::chrono::seconds(3)));

  std::vector<std::thread> senders;
  std::atomic<size_t> write_failures{0};
  senders.reserve(kWriterCount);
  for (size_t writer_index = 0; writer_index < kWriterCount; ++writer_index) {
    senders.emplace_back([&, writer_index]() {
      for (size_t seq = 0; seq < kMessagesPerWriter; ++seq) {
        auto msg = std::make_shared<Chatter>();
        msg->set_seq(seq);
        msg->set_content(payloads[writer_index]);
        msg->set_timestamp(writer_index);
        msg->set_lidar_timestamp(seq + 500);
        if (!writers[writer_index]->Write(msg)) {
          ++write_failures;
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
        for (const auto& messages : messages_by_writer) {
          if (messages.size() != kMessagesPerWriter) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(5)));

  std::lock_guard<std::mutex> lock(mutex);
  for (size_t writer_index = 0; writer_index < kWriterCount; ++writer_index) {
    SCOPED_TRACE("writer_" + std::to_string(writer_index));
    ASSERT_EQ(messages_by_writer[writer_index].size(), kMessagesPerWriter);
    for (size_t seq = 0; seq < kMessagesPerWriter; ++seq) {
      EXPECT_EQ(messages_by_writer[writer_index][seq].seq(), seq);
      EXPECT_EQ(messages_by_writer[writer_index][seq].content(),
                payloads[writer_index]);
      EXPECT_EQ(messages_by_writer[writer_index][seq].timestamp(),
                writer_index);
      EXPECT_EQ(messages_by_writer[writer_index][seq].lidar_timestamp(),
                seq + 500);
    }
  }
}

TEST_F(ExamplesIntegrationTest, ServiceMultipleClientsRoundTrip) {
  auto server_node = apollo::cyber::CreateNode(UniqueName("service"));
  auto server = server_node->CreateService<Driver, Driver>(
      service_name_, [](const std::shared_ptr<Driver>& request,
                        std::shared_ptr<Driver>& response) {
        response->set_content(request->content());
        response->set_msg_id(request->msg_id() + 1);
        response->set_timestamp(request->timestamp() + 10);
      });
  ASSERT_NE(server, nullptr);

  constexpr size_t kClientCount = 3;
  constexpr size_t kRequestsPerClient = 8;
  std::vector<std::shared_ptr<apollo::cyber::Node>> client_nodes;
  std::vector<std::shared_ptr<apollo::cyber::Client<Driver, Driver>>> clients;
  client_nodes.reserve(kClientCount);
  clients.reserve(kClientCount);
  for (size_t client_index = 0; client_index < kClientCount; ++client_index) {
    client_nodes.emplace_back(apollo::cyber::CreateNode(
        UniqueName("client_" + std::to_string(client_index))));
    auto client =
        client_nodes.back()->CreateClient<Driver, Driver>(service_name_);
    ASSERT_NE(client, nullptr);
    clients.emplace_back(std::move(client));
  }

  for (size_t client_index = 0; client_index < kClientCount; ++client_index) {
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
    EXPECT_EQ(response->content(), warmup->content());
    EXPECT_EQ(response->msg_id(), warmup->msg_id() + 1);
    EXPECT_EQ(response->timestamp(), warmup->timestamp() + 10);
  }

  std::atomic<size_t> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kClientCount);
  for (size_t client_index = 0; client_index < kClientCount; ++client_index) {
    workers.emplace_back([&, client_index]() {
      for (size_t request_index = 0; request_index < kRequestsPerClient;
           ++request_index) {
        auto request = std::make_shared<Driver>();
        request->set_content(MakePayload(
            256 + client_index * 64 + request_index * 17,
            static_cast<uint8_t>(client_index * 10 + request_index + 1)));
        request->set_msg_id(client_index * 100 + request_index);
        request->set_timestamp(client_index * 1000 + request_index);
        auto response = clients[client_index]->SendRequest(request);
        if (response == nullptr || response->content() != request->content() ||
            response->msg_id() != request->msg_id() + 1 ||
            response->timestamp() != request->timestamp() + 10) {
          ++failures;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(failures.load(), 0U);
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

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

#include "cyber/transport/transport.h"

#include <memory>
#include <chrono>
#include <mutex>
#include <sys/wait.h>
#include <typeinfo>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#include <vector>

#include "gtest/gtest.h"

#include "cyber/proto/unit_test.pb.h"

#include "cyber/init.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/common/identity.h"

namespace apollo {
namespace cyber {
namespace transport {

using TransmitterPtr = std::shared_ptr<Transmitter<proto::UnitTest>>;
using ReceiverPtr = std::shared_ptr<Receiver<proto::UnitTest>>;

TEST(TransportTest, constructor) {
  auto transport_a = Transport::Instance();
  auto transport_b = Transport::Instance();
  EXPECT_EQ(transport_a->participant(), transport_b->participant());
}

TEST(TransportTest, create_transmitter) {
  QosProfileConf qos_conf;
  (void)qos_conf;

  RoleAttributes attr;
  attr.set_channel_name("create_transmitter");
  Identity id;
  attr.set_id(id.HashValue());

  TransmitterPtr intra =
      Transport::Instance()->CreateTransmitter<proto::UnitTest>(
          attr, OptionalMode::INTRA);
  EXPECT_EQ(typeid(*intra), typeid(IntraTransmitter<proto::UnitTest>));

  TransmitterPtr shm =
      Transport::Instance()->CreateTransmitter<proto::UnitTest>(
          attr, OptionalMode::SHM);
  EXPECT_EQ(typeid(*shm), typeid(ShmTransmitter<proto::UnitTest>));

  TransmitterPtr iceoryx =
      Transport::Instance()->CreateTransmitter<proto::UnitTest>(
          attr, OptionalMode::ICEORYX);
  EXPECT_EQ(typeid(*iceoryx), typeid(IceoryxTransmitter<proto::UnitTest>));
}

TEST(TransportTest, create_receiver) {
  RoleAttributes attr;
  attr.set_channel_name("create_receiver");
  Identity id;
  attr.set_id(id.HashValue());

  auto listener = [](const std::shared_ptr<proto::UnitTest>&,
                     const MessageInfo&, const RoleAttributes&) {};

  ReceiverPtr intra = Transport::Instance()->CreateReceiver<proto::UnitTest>(
      attr, listener, OptionalMode::INTRA);
  EXPECT_EQ(typeid(*intra), typeid(IntraReceiver<proto::UnitTest>));

  ReceiverPtr shm = Transport::Instance()->CreateReceiver<proto::UnitTest>(
      attr, listener, OptionalMode::SHM);
  EXPECT_EQ(typeid(*shm), typeid(ShmReceiver<proto::UnitTest>));

  ReceiverPtr iceoryx = Transport::Instance()->CreateReceiver<proto::UnitTest>(
      attr, listener, OptionalMode::ICEORYX);
  EXPECT_EQ(typeid(*iceoryx), typeid(IceoryxReceiver<proto::UnitTest>));
}

TEST(TransportTest, iceoryx_end_to_end) {
  RoleAttributes attr;
  attr.set_channel_name("iceoryx_end_to_end");
  Identity id;
  attr.set_id(id.HashValue());
  attr.mutable_qos_profile()->set_depth(2);

  std::mutex mtx;
  std::vector<proto::UnitTest> msgs;
  auto listener = [&](const std::shared_ptr<proto::UnitTest>& msg,
                      const MessageInfo&, const RoleAttributes&) {
    std::lock_guard<std::mutex> lock(mtx);
    msgs.push_back(*msg);
  };

  auto transmitter = Transport::Instance()->CreateTransmitter<proto::UnitTest>(
      attr, OptionalMode::ICEORYX);
  auto receiver = Transport::Instance()->CreateReceiver<proto::UnitTest>(
      attr, listener, OptionalMode::ICEORYX);

  ASSERT_NE(transmitter, nullptr);
  ASSERT_NE(receiver, nullptr);

  receiver->Enable();
  transmitter->Enable(receiver->attributes());

  auto msg = std::make_shared<proto::UnitTest>();
  msg->set_class_name("iceoryx");
  msg->set_case_name("end_to_end");
  EXPECT_TRUE(transmitter->Transmit(msg));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
    std::lock_guard<std::mutex> lock(mtx);
    ASSERT_FALSE(msgs.empty());
    EXPECT_EQ(msgs.back().class_name(), "iceoryx");
    EXPECT_EQ(msgs.back().case_name(), "end_to_end");
  }

  transmitter->Disable(receiver->attributes());
  receiver->Disable();
}

TEST(TransportTest, iceoryx_pod_end_to_end) {
  RoleAttributes attr;
  attr.set_channel_name("iceoryx_pod_end_to_end");
  Identity id;
  attr.set_id(id.HashValue());
  attr.mutable_qos_profile()->set_depth(2);

  std::mutex mtx;
  std::vector<PodMessage> msgs;
  auto listener = [&](const std::shared_ptr<PodMessage>& msg, const MessageInfo&,
                      const RoleAttributes&) {
    std::lock_guard<std::mutex> lock(mtx);
    msgs.push_back(*msg);
  };

  auto transmitter = Transport::Instance()->CreateTransmitter<PodMessage>(
      attr, OptionalMode::ICEORYX);
  auto receiver = Transport::Instance()->CreateReceiver<PodMessage>(
      attr, listener, OptionalMode::ICEORYX);

  ASSERT_NE(transmitter, nullptr);
  ASSERT_NE(receiver, nullptr);

  receiver->Enable();
  transmitter->Enable(receiver->attributes());

  const char payload[] = "pod_zero_copy";
  PodChunkHeader header = MakeImagePodChunkHeader(
      123456789ull, 7ull, 2u, 2u, 4u, 1u, sizeof(payload));
  auto msg = std::make_shared<PodMessage>(header, payload, sizeof(payload));
  ASSERT_NE(msg, nullptr);

  EXPECT_TRUE(transmitter->Transmit(msg));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
    std::lock_guard<std::mutex> lock(mtx);
    ASSERT_FALSE(msgs.empty());
    const auto view = msgs.back().View();
    ASSERT_NE(view.payload, nullptr);
    EXPECT_EQ(view.header.timestamp_ns, 123456789ull);
    EXPECT_EQ(view.header.frame_id, 7ull);
    EXPECT_EQ(view.payload_size, sizeof(payload));
    EXPECT_EQ(std::memcmp(view.payload, payload, sizeof(payload)), 0);
  }

  transmitter->Disable(receiver->attributes());
  receiver->Disable();
}

TEST(TransportTest, hybrid_pod_interprocess_end_to_end) {
  const std::string channel_name = "hybrid_pod_interprocess_end_to_end";
  const int parent_pid = getpid();

  RoleAttributes receiver_attr;
  receiver_attr.set_host_ip(common::GlobalData::Instance()->HostIp());
  receiver_attr.set_process_id(parent_pid);
  receiver_attr.set_channel_name(channel_name);
  receiver_attr.set_channel_id(common::Hash(channel_name));
  receiver_attr.mutable_qos_profile()->set_depth(2);

  std::mutex mutex;
  std::vector<PodMessage> messages;
  auto receiver = Transport::Instance()->CreateReceiver<PodMessage>(
      receiver_attr,
      [&](const std::shared_ptr<PodMessage>& msg, const MessageInfo&,
          const RoleAttributes&) {
        std::lock_guard<std::mutex> lock(mutex);
        messages.push_back(*msg);
      },
      OptionalMode::HYBRID);
  ASSERT_NE(receiver, nullptr);
  receiver->Enable();

  const pid_t child_pid = fork();
  ASSERT_GE(child_pid, 0);
  if (child_pid == 0) {
    const std::string parent_pid_text = std::to_string(parent_pid);
    setenv("CYBER_TEST_PARENT_PID", parent_pid_text.c_str(), 1);
    execl("/proc/self/exe", "transport_test",
          "--gtest_filter=TransportTest.hybrid_pod_interprocess_transmitter",
          nullptr);
    _exit(127);
  }

  for (int i = 0; i < 40; ++i) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!messages.empty()) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  int child_status = 0;
  ASSERT_EQ(waitpid(child_pid, &child_status, 0), child_pid);
  ASSERT_TRUE(WIFEXITED(child_status));
  ASSERT_EQ(WEXITSTATUS(child_status), 0);
  {
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(messages.size(), 1u);
    const auto view = messages.front().View();
    ASSERT_NE(view.payload, nullptr);
    EXPECT_EQ(view.header.timestamp_ns, 135792468ull);
    EXPECT_EQ(view.header.frame_id, 11ull);
    const char payload[] = "hybrid_interprocess_pod";
    EXPECT_EQ(view.payload_size, sizeof(payload));
    EXPECT_EQ(std::memcmp(view.payload, payload, sizeof(payload)), 0);
  }

  receiver->Disable();
}

TEST(TransportTest, hybrid_pod_interprocess_transmitter) {
  const char* parent_pid_env = std::getenv("CYBER_TEST_PARENT_PID");
  if (parent_pid_env == nullptr) {
    GTEST_SKIP() << "worker test is launched by the parent process";
  }

  const std::string channel_name = "hybrid_pod_interprocess_end_to_end";
  RoleAttributes transmitter_attr;
  transmitter_attr.set_host_ip(common::GlobalData::Instance()->HostIp());
  transmitter_attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  transmitter_attr.set_channel_name(channel_name);
  transmitter_attr.set_channel_id(common::Hash(channel_name));
  transmitter_attr.mutable_qos_profile()->set_depth(2);

  RoleAttributes receiver_attr;
  receiver_attr.set_host_ip(common::GlobalData::Instance()->HostIp());
  receiver_attr.set_process_id(std::strtol(parent_pid_env, nullptr, 10));
  receiver_attr.set_channel_name(channel_name);
  receiver_attr.set_channel_id(common::Hash(channel_name));
  receiver_attr.mutable_qos_profile()->set_depth(2);

  auto transmitter = Transport::Instance()->CreateTransmitter<PodMessage>(
      transmitter_attr, OptionalMode::HYBRID);
  ASSERT_NE(transmitter, nullptr);
  transmitter->Enable(receiver_attr);

  const char payload[] = "hybrid_interprocess_pod";
  const PodChunkHeader header = MakeImagePodChunkHeader(
      135792468ull, 11ull, 2u, 2u, 4u, 1u, sizeof(payload));
  const auto msg = std::make_shared<PodMessage>(header, payload, sizeof(payload));
  EXPECT_TRUE(transmitter->Transmit(msg));
  transmitter->Disable(receiver_attr);
}

TEST(TransportTest, iceoryx_pod_loan_publish_end_to_end) {
  RoleAttributes attr;
  attr.set_channel_name("iceoryx_pod_loan_publish_end_to_end");
  Identity id;
  attr.set_id(id.HashValue());
  attr.mutable_qos_profile()->set_depth(2);

  std::mutex mtx;
  std::vector<PodMessage> msgs;
  auto listener = [&](const std::shared_ptr<PodMessage>& msg, const MessageInfo&,
                      const RoleAttributes&) {
    std::lock_guard<std::mutex> lock(mtx);
    msgs.push_back(*msg);
  };

  auto transmitter = Transport::Instance()->CreateTransmitter<PodMessage>(
      attr, OptionalMode::ICEORYX);
  auto receiver = Transport::Instance()->CreateReceiver<PodMessage>(
      attr, listener, OptionalMode::ICEORYX);

  ASSERT_NE(transmitter, nullptr);
  ASSERT_NE(receiver, nullptr);

  receiver->Enable();
  transmitter->Enable(receiver->attributes());

  const char payload[] = "pod_loan_publish";
  PodChunkHeader header = MakeImagePodChunkHeader(
      987654321ull, 9ull, 2u, 2u, 4u, 1u, sizeof(payload));
  LoanedMessage<PodMessage> loaned;
  ASSERT_TRUE(transmitter->Loan(PodChunkTotalSize(sizeof(payload)), &loaned));
  std::size_t written = 0;
  ASSERT_TRUE(BuildPodChunk(header, payload, sizeof(payload), loaned.data(),
                            loaned.capacity(), &written));
  ASSERT_TRUE(loaned.set_size(written));
  EXPECT_TRUE(transmitter->Publish(std::move(loaned)));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
    std::lock_guard<std::mutex> lock(mtx);
    ASSERT_FALSE(msgs.empty());
    const auto view = msgs.back().View();
    ASSERT_NE(view.payload, nullptr);
    EXPECT_EQ(view.header.timestamp_ns, 987654321ull);
    EXPECT_EQ(view.header.frame_id, 9ull);
    EXPECT_EQ(view.payload_size, sizeof(payload));
    EXPECT_EQ(std::memcmp(view.payload, payload, sizeof(payload)), 0);
  }

  transmitter->Disable(receiver->attributes());
  receiver->Disable();
}

TEST(TransportTest, iceoryx_pod_large_loan_borrow_lifetime) {
  RoleAttributes attr;
  attr.set_channel_name("iceoryx_pod_large_loan_borrow_lifetime");
  Identity id;
  attr.set_id(id.HashValue());
  attr.mutable_qos_profile()->set_depth(4);

  std::mutex mtx;
  std::shared_ptr<PodMessage> held_msg;
  auto listener = [&](const std::shared_ptr<PodMessage>& msg, const MessageInfo&,
                      const RoleAttributes&) {
    std::lock_guard<std::mutex> lock(mtx);
    held_msg = msg;
  };

  auto transmitter = Transport::Instance()->CreateTransmitter<PodMessage>(
      attr, OptionalMode::ICEORYX);
  auto receiver = Transport::Instance()->CreateReceiver<PodMessage>(
      attr, listener, OptionalMode::ICEORYX);

  ASSERT_NE(transmitter, nullptr);
  ASSERT_NE(receiver, nullptr);

  receiver->Enable();
  transmitter->Enable(receiver->attributes());

  constexpr std::size_t kPayloadSize = 5u * 1024u * 1024u;
  std::vector<uint8_t> payload(kPayloadSize);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 131u + 17u) & 0xFFu);
  }

  PodChunkHeader header = MakeImagePodChunkHeader(
      1122334455ull, 42ull, 1920u, 1080u, 1920u * 3u, 1u,
      static_cast<uint32_t>(payload.size()));
  LoanedMessage<PodMessage> loaned;
  ASSERT_TRUE(transmitter->Loan(PodChunkTotalSize(payload.size()), &loaned));
  std::size_t written = 0;
  ASSERT_TRUE(BuildPodChunk(header, payload.data(), payload.size(),
                            loaned.data(), loaned.capacity(), &written));
  ASSERT_TRUE(loaned.set_size(written));
  ASSERT_TRUE(transmitter->Publish(std::move(loaned)));

  for (int i = 0; i < 50; ++i) {
    {
      std::lock_guard<std::mutex> lock(mtx);
      if (held_msg != nullptr) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  std::shared_ptr<PodMessage> local_msg;
  {
    std::lock_guard<std::mutex> lock(mtx);
    ASSERT_NE(held_msg, nullptr);
    local_msg = held_msg;
  }

  transmitter->Disable(receiver->attributes());
  receiver->Disable();

  const auto view = local_msg->View();
  ASSERT_NE(view.payload, nullptr);
  EXPECT_EQ(view.header.timestamp_ns, 1122334455ull);
  EXPECT_EQ(view.header.frame_id, 42ull);
  EXPECT_EQ(view.payload_size, payload.size());
  EXPECT_EQ(view.payload[0], payload[0]);
  EXPECT_EQ(view.payload[payload.size() / 2], payload[payload.size() / 2]);
  EXPECT_EQ(view.payload[payload.size() - 1], payload[payload.size() - 1]);
}

TEST(TransportTest, iceoryx_pod_loan_reports_mempool_exhaustion) {
  RoleAttributes attr;
  attr.set_channel_name("iceoryx_pod_loan_reports_mempool_exhaustion");
  Identity id;
  attr.set_id(id.HashValue());
  attr.mutable_qos_profile()->set_depth(1);

  auto transmitter = Transport::Instance()->CreateTransmitter<PodMessage>(
      attr, OptionalMode::ICEORYX);
  ASSERT_NE(transmitter, nullptr);
  transmitter->Enable();

  constexpr std::size_t kPayloadSize = 5u * 1024u * 1024u;
  std::vector<LoanedMessage<PodMessage>> held_loans;
  held_loans.reserve(64);
  std::size_t successful_loans = 0;
  for (std::size_t i = 0; i < 64; ++i) {
    LoanedMessage<PodMessage> loaned;
    if (!transmitter->Loan(PodChunkTotalSize(kPayloadSize), &loaned)) {
      break;
    }
    ++successful_loans;
    held_loans.emplace_back(std::move(loaned));
  }

  EXPECT_GT(successful_loans, 0U);
  EXPECT_LT(successful_loans, 64U);
  held_loans.clear();
  transmitter->Disable();
}

TEST(TransportTest, iceoryx_pod_slow_consumer_does_not_block_publisher) {
  RoleAttributes attr;
  attr.set_channel_name("iceoryx_pod_slow_consumer_does_not_block_publisher");
  Identity id;
  attr.set_id(id.HashValue());
  attr.mutable_qos_profile()->set_depth(1);

  std::atomic<std::size_t> received{0};
  auto listener = [&](const std::shared_ptr<PodMessage>& msg, const MessageInfo&,
                      const RoleAttributes&) {
    if (msg != nullptr) {
      received.fetch_add(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };

  auto transmitter = Transport::Instance()->CreateTransmitter<PodMessage>(
      attr, OptionalMode::ICEORYX);
  auto receiver = Transport::Instance()->CreateReceiver<PodMessage>(
      attr, listener, OptionalMode::ICEORYX);

  ASSERT_NE(transmitter, nullptr);
  ASSERT_NE(receiver, nullptr);

  receiver->Enable();
  transmitter->Enable(receiver->attributes());

  const char payload[] = "slow_consumer";
  PodChunkHeader header = MakeImagePodChunkHeader(
      99887766ull, 1ull, 2u, 2u, 4u, 1u, sizeof(payload));
  auto msg = std::make_shared<PodMessage>(header, payload, sizeof(payload));

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 50; ++i) {
    EXPECT_TRUE(transmitter->Transmit(msg));
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            500);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GT(received.load(), 0U);

  transmitter->Disable(receiver->attributes());
  receiver->Disable();
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  const auto cyber_path = (std::filesystem::current_path() / "cyber").string();
  setenv("CYBER_PATH", cyber_path.c_str(), 1);
  testing::InitGoogleTest(&argc, argv);
  apollo::cyber::Init(argv[0]);
  apollo::cyber::transport::Transport::Instance();
  auto res = RUN_ALL_TESTS();
  apollo::cyber::transport::Transport::Instance()->Shutdown();
  return res;
}

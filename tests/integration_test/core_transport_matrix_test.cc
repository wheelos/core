// Copyright 2026 WheelOS. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "gtest/gtest.h"

#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/cyber.h"
#include "cyber/proto/unit_test.pb.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/transport.h"

namespace apollo {
namespace cyber {
namespace {

using transport::MessageInfo;
using transport::OptionalMode;
using transport::PodChunkHeader;
using transport::PodMessage;
using transport::RoleAttributes;
using transport::Transport;

RoleAttributes MakeRole(const std::string& channel_name) {
  RoleAttributes attr;
  attr.set_host_name(common::GlobalData::Instance()->HostName());
  attr.set_host_ip(common::GlobalData::Instance()->HostIp());
  attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  attr.set_channel_name(channel_name);
  attr.set_channel_id(common::Hash(channel_name));
  attr.mutable_qos_profile()->set_depth(8);
  return attr;
}

TEST(CoreTransportMatrixTest, ProtobufSharedMemoryRoundTripIsBounded) {
  const auto attr =
      MakeRole("core_transport_shm_" + std::to_string(::getpid()));
  std::mutex mutex;
  std::condition_variable received_cv;
  std::shared_ptr<proto::UnitTest> received;

  auto receiver = Transport::Instance()->CreateReceiver<proto::UnitTest>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& message, const MessageInfo&,
          const RoleAttributes&) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          received = message;
        }
        received_cv.notify_one();
      },
      OptionalMode::SHM);
  auto transmitter = Transport::Instance()->CreateTransmitter<proto::UnitTest>(
      attr, OptionalMode::SHM);
  ASSERT_NE(receiver, nullptr);
  ASSERT_NE(transmitter, nullptr);

  receiver->Enable();
  transmitter->Enable(receiver->attributes());

  auto message = std::make_shared<proto::UnitTest>();
  message->set_class_name("core-transport-matrix");
  message->set_case_name("protobuf-shm");

  std::unique_lock<std::mutex> lock(mutex);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (received == nullptr && std::chrono::steady_clock::now() < deadline) {
    lock.unlock();
    ASSERT_TRUE(transmitter->Transmit(message));
    lock.lock();
    received_cv.wait_for(lock, std::chrono::milliseconds(100),
                         [&]() { return received != nullptr; });
  }
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->class_name(), message->class_name());
  EXPECT_EQ(received->case_name(), message->case_name());
  lock.unlock();

  transmitter->Disable(receiver->attributes());
  receiver->Disable();
}

TEST(CoreTransportMatrixTest, PodIceoryxLoanRoundTripIsBounded) {
  const auto attr =
      MakeRole("core_transport_iceoryx_" + std::to_string(::getpid()));
  std::mutex mutex;
  std::condition_variable received_cv;
  std::shared_ptr<PodMessage> received;

  auto receiver = Transport::Instance()->CreateReceiver<PodMessage>(
      attr,
      [&](const std::shared_ptr<PodMessage>& message, const MessageInfo&,
          const RoleAttributes&) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          received = message;
        }
        received_cv.notify_one();
      },
      OptionalMode::ICEORYX);
  auto transmitter = Transport::Instance()->CreateTransmitter<PodMessage>(
      attr, OptionalMode::ICEORYX);
  ASSERT_NE(receiver, nullptr);
  ASSERT_NE(transmitter, nullptr);

  receiver->Enable();
  transmitter->Enable(receiver->attributes());

  constexpr char kPayload[] = "core-matrix-pod";
  const PodChunkHeader header = transport::MakeImagePodChunkHeader(
      123456789, 42, 2, 2, 4, 1, sizeof(kPayload));
  transport::LoanedMessage<PodMessage> loaned;
  ASSERT_TRUE(
      transmitter->Loan(transport::PodChunkTotalSize(sizeof(kPayload)),
                        &loaned));
  std::size_t written = 0;
  ASSERT_TRUE(transport::BuildPodChunk(header, kPayload, sizeof(kPayload),
                                      loaned.data(), loaned.capacity(),
                                      &written));
  ASSERT_TRUE(loaned.set_size(written));
  ASSERT_TRUE(transmitter->Publish(std::move(loaned)));

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(received_cv.wait_for(lock, std::chrono::seconds(5),
                                   [&]() { return received != nullptr; }));
  const auto view = received->View();
  ASSERT_NE(view.payload, nullptr);
  EXPECT_EQ(view.header.timestamp_ns, header.timestamp_ns);
  EXPECT_EQ(view.header.frame_id, header.frame_id);
  EXPECT_EQ(view.payload_size, sizeof(kPayload));
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(view.payload),
                        view.payload_size),
            std::string(kPayload, sizeof(kPayload)));
  lock.unlock();

  transmitter->Disable(receiver->attributes());
  receiver->Disable();
}

}  // namespace
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

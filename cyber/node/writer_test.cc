/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
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

#include "cyber/node/writer.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "gtest/gtest.h"

#include "cyber/proto/unit_test.pb.h"

#include "cyber/cyber.h"
#include "cyber/init.h"

namespace apollo {
namespace cyber {

class WriterTestPeer {
 public:
  template <typename MessageT>
  static void SetTransmitter(
      Writer<MessageT>* writer,
      const std::shared_ptr<transport::Transmitter<MessageT>>& transmitter) {
    std::lock_guard<std::mutex> lock(writer->lock_);
    writer->transmitter_ = transmitter;
  }
};

namespace writer {

using proto::Chatter;

class BlockingTransmitState {
 public:
  void WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return entered_; });
  }

  void EnterAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return released_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

class BlockingTransmitter : public transport::Transmitter<Chatter> {
 public:
  BlockingTransmitter(const proto::RoleAttributes& role,
                      const std::shared_ptr<BlockingTransmitState>& state)
      : transport::Transmitter<Chatter>(role), state_(state) {}

  void Enable() override {}
  void Disable() override {}

  bool Transmit(const MessagePtr& msg,
                const transport::MessageInfo& msg_info) override {
    (void)msg;
    (void)msg_info;
    Block();
    return true;
  }

  bool Loan(std::size_t size,
            transport::LoanedMessage<Chatter>* loaned_msg) override {
    (void)size;
    (void)loaned_msg;
    Block();
    return true;
  }

  bool Publish(transport::LoanedMessage<Chatter>&& loaned_msg) override {
    (void)loaned_msg;
    Block();
    return true;
  }

 private:
  void Block() {
    auto state = state_;
    state->EnterAndWait();
  }

  std::shared_ptr<BlockingTransmitState> state_;
};

template <typename Operation>
void VerifyShutdownKeepsInFlightTransmitterAlive(
    const std::string& channel_name, Operation operation) {
  proto::RoleAttributes role;
  role.set_channel_name(channel_name);
  role.set_node_name("writer_shutdown_race_node");

  Writer<Chatter> writer(role);
  ASSERT_TRUE(writer.Init());

  auto state = std::make_shared<BlockingTransmitState>();
  auto transmitter = std::make_shared<BlockingTransmitter>(role, state);
  std::weak_ptr<BlockingTransmitter> weak_transmitter = transmitter;
  WriterTestPeer::SetTransmitter(
      &writer,
      std::static_pointer_cast<transport::Transmitter<Chatter>>(transmitter));
  transmitter.reset();

  bool operation_result = false;
  std::thread operation_thread(
      [&]() { operation_result = operation(&writer); });
  state->WaitUntilEntered();

  writer.Shutdown();
  EXPECT_FALSE(weak_transmitter.expired());

  state->Release();
  operation_thread.join();

  EXPECT_TRUE(operation_result);
  EXPECT_TRUE(weak_transmitter.expired());
}

TEST(WriterTest, test1) {
  proto::RoleAttributes role;
  Writer<Chatter> w(role);
  EXPECT_TRUE(w.Init());
  EXPECT_TRUE(w.IsInit());
  EXPECT_TRUE(w.GetChannelName().empty());
  EXPECT_FALSE(w.HasReader());
}

TEST(WriterTest, test2) {
  proto::RoleAttributes role;
  auto qos = role.mutable_qos_profile();
  qos->set_history(proto::QosHistoryPolicy::HISTORY_KEEP_LAST);
  qos->set_depth(0);
  qos->set_mps(0);
  qos->set_reliability(proto::QosReliabilityPolicy::RELIABILITY_RELIABLE);
  qos->set_durability(proto::QosDurabilityPolicy::DURABILITY_VOLATILE);
  role.set_channel_name("/chatter0");
  role.set_node_name("chatter_node");

  Writer<Chatter> w(role);
  EXPECT_TRUE(w.Init());
  EXPECT_EQ(w.GetChannelName(), "/chatter0");

  {
    auto c = std::make_shared<Chatter>();
    c->set_timestamp(Time::Now().ToNanosecond());
    c->set_lidar_timestamp(Time::Now().ToNanosecond());
    c->set_seq(3);
    c->set_content("ChatterMsg");
    EXPECT_TRUE(w.Write(c));
  }
  EXPECT_TRUE(w.Init());

  w.Shutdown();

  auto c = std::make_shared<Chatter>();
  c->set_timestamp(Time::Now().ToNanosecond());
  c->set_lidar_timestamp(Time::Now().ToNanosecond());
  c->set_seq(3);
  c->set_content("ChatterMsg");
  EXPECT_FALSE(w.Write(c));
}

TEST(WriterTest, ShutdownKeepsInFlightSharedWriteTransmitterAlive) {
  auto message = std::make_shared<Chatter>();
  VerifyShutdownKeepsInFlightTransmitterAlive(
      "/writer_shutdown_write_race",
      [&](Writer<Chatter>* writer) { return writer->Write(message); });
}

TEST(WriterTest, ShutdownKeepsInFlightValueWriteTransmitterAlive) {
  Chatter message;
  VerifyShutdownKeepsInFlightTransmitterAlive(
      "/writer_shutdown_value_write_race",
      [&](Writer<Chatter>* writer) { return writer->Write(message); });
}

TEST(WriterTest, ShutdownKeepsInFlightLoanTransmitterAlive) {
  transport::LoanedMessage<Chatter> loaned_message;
  VerifyShutdownKeepsInFlightTransmitterAlive(
      "/writer_shutdown_loan_race", [&](Writer<Chatter>* writer) {
        return writer->Loan(1, &loaned_message);
      });
}

TEST(WriterTest, ShutdownKeepsInFlightPublishTransmitterAlive) {
  transport::LoanedMessage<Chatter> loaned_message;
  VerifyShutdownKeepsInFlightTransmitterAlive(
      "/writer_shutdown_publish_race", [&](Writer<Chatter>* writer) {
        return writer->Publish(std::move(loaned_message));
      });
}

}  // namespace writer
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  apollo::cyber::Init(argv[0]);
  return RUN_ALL_TESTS();
}

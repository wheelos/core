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

#include <condition_variable>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "gtest/gtest.h"

#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/init.h"
#include "cyber/proto/unit_test.pb.h"
#include "cyber/transport/qos/qos_profile_conf.h"
#include "cyber/transport/receiver/hybrid_receiver.h"
#include "cyber/transport/transmitter/hybrid_transmitter.h"
#include "cyber/transport/transport.h"

namespace apollo {
namespace cyber {
namespace transport {

class HybridTransmitterTestPeer {
 public:
  template <typename M>
  static void SetTransmitter(HybridTransmitter<M>* hybrid,
                             const RoleAttributes& opposite_attr,
                             const std::shared_ptr<Transmitter<M>>&
                                 transmitter) {
    auto relation = hybrid->GetRelation(opposite_attr);
    auto mode = hybrid->mapping_table_[relation];
    std::lock_guard<std::mutex> lock(hybrid->mutex_);
    hybrid->transmitters_[mode] = transmitter;
  }

  template <typename M>
  static bool HasReceiver(HybridTransmitter<M>* hybrid, uint64_t id) {
    std::lock_guard<std::mutex> lock(hybrid->mutex_);
    for (const auto& item : hybrid->receivers_) {
      if (item.second.count(id) != 0) {
        return true;
      }
    }
    return false;
  }
};

class BlockingEnableTransmitter : public Transmitter<proto::UnitTest> {
 public:
  explicit BlockingEnableTransmitter(const RoleAttributes& attr)
      : Transmitter<proto::UnitTest>(attr) {}

  void Enable() override {
    std::unique_lock<std::mutex> lock(mutex_);
    enable_entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return release_enable_; });
  }

  void Disable() override {}

  bool Transmit(const MessagePtr& msg,
                const MessageInfo& msg_info) override {
    (void)msg;
    (void)msg_info;
    return true;
  }

  void WaitUntilEnableBlocked() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return enable_entered_; });
  }

  void ReleaseEnable() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_enable_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool enable_entered_ = false;
  bool release_enable_ = false;
};

class HybridTransceiverTest : public ::testing::Test {
 protected:
  using TransmitterPtr = std::shared_ptr<Transmitter<proto::UnitTest>>;
  using ReceiverPtr = std::shared_ptr<Receiver<proto::UnitTest>>;

  HybridTransceiverTest() : channel_name_("hybrid_channel") {}

  virtual ~HybridTransceiverTest() {}

  virtual void SetUp() {
    RoleAttributes attr;
    attr.set_host_name(common::GlobalData::Instance()->HostName());
    attr.set_host_ip(common::GlobalData::Instance()->HostIp());
    attr.set_process_id(common::GlobalData::Instance()->ProcessId());
    attr.set_channel_name(channel_name_);
    attr.set_channel_id(common::Hash(channel_name_));
    attr.mutable_qos_profile()->CopyFrom(QosProfileConf::QOS_PROFILE_DEFAULT);
    transmitter_a_ = std::make_shared<HybridTransmitter<proto::UnitTest>>(
        attr, Transport::Instance()->participant());

    attr.set_process_id(common::GlobalData::Instance()->ProcessId() + 1);
    attr.mutable_qos_profile()->CopyFrom(QosProfileConf::QOS_PROFILE_DEFAULT);
    transmitter_b_ = std::make_shared<HybridTransmitter<proto::UnitTest>>(
        attr, Transport::Instance()->participant());
  }

  virtual void TearDown() {
    transmitter_a_ = nullptr;
    transmitter_b_ = nullptr;
  }

  std::string channel_name_;
  TransmitterPtr transmitter_a_ = nullptr;
  TransmitterPtr transmitter_b_ = nullptr;
};

TEST_F(HybridTransceiverTest, constructor) {
  RoleAttributes attr;
  TransmitterPtr transmitter =
      std::make_shared<HybridTransmitter<proto::UnitTest>>(
          attr, Transport::Instance()->participant());
  ReceiverPtr receiver = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr, nullptr, Transport::Instance()->participant());

  EXPECT_EQ(transmitter->seq_num(), 0);

  auto& transmitter_id = transmitter->id();
  auto& receiver_id = receiver->id();

  EXPECT_NE(transmitter_id.ToString(), receiver_id.ToString());
}

TEST_F(HybridTransceiverTest, enable_and_disable_with_param_no_relation) {
  RoleAttributes attr;
  attr.set_host_name(common::GlobalData::Instance()->HostName());
  attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  attr.mutable_qos_profile()->CopyFrom(QosProfileConf::QOS_PROFILE_DEFAULT);
  attr.set_channel_name("enable_and_disable_with_param_no_relation");
  attr.set_channel_id(
      common::Hash("enable_and_disable_with_param_no_relation"));

  std::mutex mtx;
  std::vector<proto::UnitTest> msgs;
  ReceiverPtr receiver_a = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& attr) {
        (void)msg_info;
        (void)attr;
        std::lock_guard<std::mutex> lock(mtx);
        msgs.emplace_back(*msg);
      },
      Transport::Instance()->participant());

  ReceiverPtr receiver_b = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& attr) {
        (void)msg_info;
        (void)attr;
        std::lock_guard<std::mutex> lock(mtx);
        msgs.emplace_back(*msg);
      },
      Transport::Instance()->participant());

  auto msg = std::make_shared<proto::UnitTest>();
  msg->set_class_name("HybridTransceiverTest");
  msg->set_case_name("enable_and_disable_with_param_no_relation");

  transmitter_a_->Enable(receiver_a->attributes());
  transmitter_a_->Enable(receiver_b->attributes());
  receiver_a->Enable(transmitter_a_->attributes());
  receiver_b->Enable(transmitter_a_->attributes());

  transmitter_a_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(msgs.size(), 0);

  msgs.clear();
  transmitter_a_->Disable(receiver_a->attributes());
  transmitter_a_->Disable(receiver_b->attributes());
  receiver_a->Disable(transmitter_a_->attributes());
  receiver_b->Disable(transmitter_a_->attributes());
}

TEST_F(HybridTransceiverTest, enable_and_disable_with_param_same_process) {
  RoleAttributes attr;
  attr.set_host_name(common::GlobalData::Instance()->HostName());
  attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  attr.mutable_qos_profile()->CopyFrom(QosProfileConf::QOS_PROFILE_DEFAULT);
  attr.set_channel_name(channel_name_);
  attr.set_channel_id(common::Hash(channel_name_));

  std::mutex mtx;
  std::vector<proto::UnitTest> msgs;
  ReceiverPtr receiver_a = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& attr) {
        (void)msg_info;
        (void)attr;
        std::lock_guard<std::mutex> lock(mtx);
        msgs.emplace_back(*msg);
      },
      Transport::Instance()->participant());

  ReceiverPtr receiver_b = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& attr) {
        (void)msg_info;
        (void)attr;
        std::lock_guard<std::mutex> lock(mtx);
        msgs.emplace_back(*msg);
      },
      Transport::Instance()->participant());

  std::string class_name("HybridTransceiverTest");
  std::string case_name("enable_and_disable_with_param_same_process");
  auto msg = std::make_shared<proto::UnitTest>();
  msg->set_class_name(class_name);
  msg->set_case_name(case_name);

  // this msg will lose
  transmitter_a_->Transmit(msg);

  transmitter_a_->Enable(receiver_a->attributes());
  transmitter_a_->Enable(receiver_b->attributes());
  receiver_a->Enable(transmitter_a_->attributes());
  receiver_b->Enable(transmitter_a_->attributes());
  // repeated call
  receiver_b->Enable(transmitter_a_->attributes());

  transmitter_a_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(msgs.size(), 2);
  for (auto& item : msgs) {
    EXPECT_EQ(item.class_name(), class_name);
    EXPECT_EQ(item.case_name(), case_name);
  }

  msgs.clear();
  transmitter_a_->Disable(receiver_a->attributes());
  transmitter_a_->Disable(receiver_b->attributes());
  receiver_a->Disable(transmitter_a_->attributes());
  receiver_b->Disable(transmitter_a_->attributes());
  // repeated call
  receiver_b->Disable(transmitter_a_->attributes());

  transmitter_a_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(msgs.size(), 0);
}

TEST_F(HybridTransceiverTest,
       departed_writer_is_disconnected_while_same_mode_writer_survives) {
  RoleAttributes attr;
  attr.set_host_name(common::GlobalData::Instance()->HostName());
  attr.set_host_ip(common::GlobalData::Instance()->HostIp());
  attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  attr.mutable_qos_profile()->CopyFrom(QosProfileConf::QOS_PROFILE_DEFAULT);
  attr.set_channel_name("departed_writer_listener");
  attr.set_channel_id(common::Hash("departed_writer_listener"));

  auto writer1 = std::make_shared<HybridTransmitter<proto::UnitTest>>(
      attr, Transport::Instance()->participant());
  auto writer2 = std::make_shared<HybridTransmitter<proto::UnitTest>>(
      attr, Transport::Instance()->participant());
  auto writer1_endpoint =
      std::static_pointer_cast<Transmitter<proto::UnitTest>>(writer1);
  auto writer2_endpoint =
      std::static_pointer_cast<Transmitter<proto::UnitTest>>(writer2);

  std::vector<std::string> received;
  auto receiver = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& receiver_attr) {
        (void)msg_info;
        (void)receiver_attr;
        received.emplace_back(msg->case_name());
      },
      Transport::Instance()->participant());

  writer1->Enable(receiver->attributes());
  writer2->Enable(receiver->attributes());
  receiver->Enable(writer1->attributes());
  receiver->Enable(writer2->attributes());

  auto msg = std::make_shared<proto::UnitTest>();
  msg->set_case_name("writer1-before-leave");
  ASSERT_TRUE(writer1_endpoint->Transmit(msg));
  msg->set_case_name("writer2-before-leave");
  ASSERT_TRUE(writer2_endpoint->Transmit(msg));
  ASSERT_EQ(received.size(), 2);

  receiver->Disable(writer1->attributes());

  msg->set_case_name("writer1-after-leave");
  ASSERT_TRUE(writer1_endpoint->Transmit(msg));
  msg->set_case_name("writer2-survives");
  ASSERT_TRUE(writer2_endpoint->Transmit(msg));
  ASSERT_EQ(received.size(), 3);
  EXPECT_EQ(received.back(), "writer2-survives");

  receiver->Disable(writer2->attributes());

  msg->set_case_name("writer1-stale-listener");
  ASSERT_TRUE(writer1_endpoint->Transmit(msg));
  msg->set_case_name("writer2-after-leave");
  ASSERT_TRUE(writer2_endpoint->Transmit(msg));
  EXPECT_EQ(received.size(), 3);
}

TEST_F(HybridTransceiverTest,
       disable_cannot_overtake_first_receiver_enable) {
  RoleAttributes transmitter_attr;
  transmitter_attr.set_id(1001);
  transmitter_attr.set_host_name(common::GlobalData::Instance()->HostName());
  transmitter_attr.set_host_ip(common::GlobalData::Instance()->HostIp());
  transmitter_attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  transmitter_attr.set_channel_name(channel_name_);
  transmitter_attr.set_channel_id(common::Hash(channel_name_));
  transmitter_attr.mutable_qos_profile()->CopyFrom(
      QosProfileConf::QOS_PROFILE_DEFAULT);

  auto transmitter = std::make_shared<HybridTransmitter<proto::UnitTest>>(
      transmitter_attr, Transport::Instance()->participant());
  RoleAttributes receiver_attr = transmitter_attr;
  receiver_attr.set_id(1002);

  auto blocking_transmitter =
      std::make_shared<BlockingEnableTransmitter>(transmitter_attr);
  HybridTransmitterTestPeer::SetTransmitter(
      transmitter.get(), receiver_attr,
      std::static_pointer_cast<Transmitter<proto::UnitTest>>(
          blocking_transmitter));

  std::thread enable_thread(
      [&]() { transmitter->Enable(receiver_attr); });
  blocking_transmitter->WaitUntilEnableBlocked();

  std::promise<void> disable_started;
  std::promise<void> disable_finished;
  auto disable_started_future = disable_started.get_future();
  auto disable_finished_future = disable_finished.get_future();
  std::thread disable_thread([&]() {
    disable_started.set_value();
    transmitter->Disable(receiver_attr);
    disable_finished.set_value();
  });

  disable_started_future.wait();
  // Enable has released the state mutex but has not inserted the receiver yet.
  // Disable must wait for that JOIN transition instead of erasing too early.
  EXPECT_EQ(disable_finished_future.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);

  blocking_transmitter->ReleaseEnable();
  enable_thread.join();
  disable_thread.join();

  EXPECT_FALSE(HybridTransmitterTestPeer::HasReceiver(
      transmitter.get(), receiver_attr.id()));
}

TEST_F(HybridTransceiverTest,
       transient_history_replay_outlives_hybrid_endpoints) {
  RoleAttributes writer_attr;
  writer_attr.set_host_name(common::GlobalData::Instance()->HostName());
  writer_attr.set_host_ip(common::GlobalData::Instance()->HostIp());
  writer_attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  writer_attr.set_channel_name("transient_history_lifetime");
  writer_attr.set_channel_id(common::Hash("transient_history_lifetime"));
  writer_attr.mutable_qos_profile()->CopyFrom(
      QosProfileConf::QOS_PROFILE_DEFAULT);
  writer_attr.mutable_qos_profile()->set_durability(
      proto::QosDurabilityPolicy::DURABILITY_TRANSIENT_LOCAL);

  RoleAttributes reader_attr(writer_attr);
  reader_attr.set_process_id(writer_attr.process_id() + 1);

  auto writer = std::make_shared<HybridTransmitter<proto::UnitTest>>(
      writer_attr, Transport::Instance()->participant());
  auto reader = std::make_shared<HybridReceiver<proto::UnitTest>>(
      reader_attr,
      [](const std::shared_ptr<proto::UnitTest>&, const MessageInfo&,
         const RoleAttributes&) {},
      Transport::Instance()->participant());

  auto writer_endpoint =
      std::static_pointer_cast<Transmitter<proto::UnitTest>>(writer);
  auto message = std::make_shared<proto::UnitTest>();
  ASSERT_TRUE(writer_endpoint->Transmit(message));

  reader->Enable(writer->attributes());
  writer->Enable(reader->attributes());
  reader.reset();
  writer.reset();

  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
}

TEST_F(HybridTransceiverTest,
       enable_and_disable_with_param_same_host_diff_proc) {
  RoleAttributes attr;
  attr.set_host_name(common::GlobalData::Instance()->HostName());
  attr.set_process_id(1);
  attr.mutable_qos_profile()->CopyFrom(QosProfileConf::QOS_PROFILE_DEFAULT);
  attr.set_channel_name(channel_name_);
  attr.set_channel_id(common::Hash(channel_name_));

  std::mutex mtx;
  std::vector<proto::UnitTest> msgs;
  ReceiverPtr receiver_a = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& attr) {
        (void)msg_info;
        (void)attr;
        std::lock_guard<std::mutex> lock(mtx);
        msgs.emplace_back(*msg);
      },
      Transport::Instance()->participant());

  ReceiverPtr receiver_b = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& attr) {
        (void)msg_info;
        (void)attr;
        std::lock_guard<std::mutex> lock(mtx);
        msgs.emplace_back(*msg);
      },
      Transport::Instance()->participant());

  std::string class_name("HybridTransceiverTest");
  std::string case_name("enable_and_disable_with_param_same_host_diff_proc");
  auto msg = std::make_shared<proto::UnitTest>();
  msg->set_class_name(class_name);
  msg->set_case_name(case_name);

  transmitter_b_->Transmit(msg);

  transmitter_b_->Enable(receiver_a->attributes());
  transmitter_b_->Enable(receiver_b->attributes());
  receiver_a->Enable(transmitter_b_->attributes());
  receiver_b->Enable(transmitter_b_->attributes());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  transmitter_b_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // 1 from receiver_b, 1 from receiver_a
  EXPECT_EQ(msgs.size(), 2);
  for (auto& item : msgs) {
    EXPECT_EQ(item.class_name(), class_name);
    EXPECT_EQ(item.case_name(), case_name);
  }

  msgs.clear();
  transmitter_b_->Disable(receiver_a->attributes());
  transmitter_b_->Disable(receiver_b->attributes());
  receiver_a->Disable(transmitter_b_->attributes());
  receiver_b->Disable(transmitter_b_->attributes());

  transmitter_b_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(msgs.size(), 0);
}

TEST_F(HybridTransceiverTest, enable_and_disable_with_param_diff_host) {
  RoleAttributes attr;
  attr.set_host_name("sorac");
  attr.set_process_id(12345);
  attr.mutable_qos_profile()->CopyFrom(QosProfileConf::QOS_PROFILE_DEFAULT);
  attr.set_channel_name(channel_name_);
  attr.set_channel_id(common::Hash(channel_name_));

  std::mutex mtx;
  std::vector<proto::UnitTest> msgs;
  ReceiverPtr receiver_a = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& attr) {
        (void)msg_info;
        (void)attr;
        std::lock_guard<std::mutex> lock(mtx);
        msgs.emplace_back(*msg);
      },
      Transport::Instance()->participant());

  ReceiverPtr receiver_b = std::make_shared<HybridReceiver<proto::UnitTest>>(
      attr,
      [&](const std::shared_ptr<proto::UnitTest>& msg,
          const MessageInfo& msg_info, const RoleAttributes& attr) {
        (void)msg_info;
        (void)attr;
        std::lock_guard<std::mutex> lock(mtx);
        msgs.emplace_back(*msg);
      },
      Transport::Instance()->participant());

  std::string class_name("HybridTransceiverTest");
  std::string case_name("enable_and_disable_with_param_same_host_diff_proc");
  auto msg = std::make_shared<proto::UnitTest>();
  msg->set_class_name(class_name);
  msg->set_case_name(case_name);

  transmitter_b_->Enable(receiver_a->attributes());
  transmitter_b_->Enable(receiver_b->attributes());
  receiver_a->Enable(transmitter_b_->attributes());
  receiver_b->Enable(transmitter_b_->attributes());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  transmitter_b_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // 1 from receiver_b, 1 from receiver_a
  EXPECT_EQ(msgs.size(), 2);
  for (auto& item : msgs) {
    EXPECT_EQ(item.class_name(), class_name);
    EXPECT_EQ(item.case_name(), case_name);
  }

  msgs.clear();
  transmitter_b_->Disable(receiver_a->attributes());
  transmitter_b_->Disable(receiver_b->attributes());
  transmitter_b_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(msgs.size(), 0);
}

TEST_F(HybridTransceiverTest,
       enable_and_disable_with_param_same_proc_and_diff_host) {
  RoleAttributes same_proc_attr;
  same_proc_attr.set_host_name(common::GlobalData::Instance()->HostName());
  same_proc_attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  same_proc_attr.mutable_qos_profile()->CopyFrom(
      QosProfileConf::QOS_PROFILE_DEFAULT);
  same_proc_attr.set_channel_name(channel_name_);
  same_proc_attr.set_channel_id(common::Hash(channel_name_));

  RoleAttributes diff_host_attr;
  diff_host_attr.set_host_name("sorac");
  diff_host_attr.set_process_id(12345);
  diff_host_attr.mutable_qos_profile()->CopyFrom(
      QosProfileConf::QOS_PROFILE_DEFAULT);
  diff_host_attr.set_channel_name(channel_name_);
  diff_host_attr.set_channel_id(common::Hash(channel_name_));

  std::mutex mtx;
  std::vector<proto::UnitTest> msgs;
  ReceiverPtr same_proc_receiver =
      std::make_shared<HybridReceiver<proto::UnitTest>>(
          same_proc_attr,
          [&](const std::shared_ptr<proto::UnitTest>& msg,
              const MessageInfo& msg_info, const RoleAttributes& attr) {
            (void)msg_info;
            (void)attr;
            std::lock_guard<std::mutex> lock(mtx);
            msgs.emplace_back(*msg);
          },
          Transport::Instance()->participant());

  ReceiverPtr diff_host_receiver =
      std::make_shared<HybridReceiver<proto::UnitTest>>(
          diff_host_attr,
          [&](const std::shared_ptr<proto::UnitTest>& msg,
              const MessageInfo& msg_info, const RoleAttributes& attr) {
            (void)msg_info;
            (void)attr;
            std::lock_guard<std::mutex> lock(mtx);
            msgs.emplace_back(*msg);
          },
          Transport::Instance()->participant());

  std::string class_name(
      "enable_and_disable_with_param_same_proc_and_diff_host");
  auto msg = std::make_shared<proto::UnitTest>();
  msg->set_class_name(class_name);
  msg->set_case_name(class_name);

  transmitter_a_->Enable(same_proc_receiver->attributes());
  transmitter_a_->Enable(diff_host_receiver->attributes());
  same_proc_receiver->Enable(transmitter_a_->attributes());
  diff_host_receiver->Enable(transmitter_a_->attributes());

  transmitter_a_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(msgs.size(), 2);
  for (auto& item : msgs) {
    EXPECT_EQ(item.class_name(), class_name);
    EXPECT_EQ(item.case_name(), class_name);
  }

  msgs.clear();
  transmitter_a_->Disable(same_proc_receiver->attributes());
  transmitter_a_->Disable(diff_host_receiver->attributes());
  same_proc_receiver->Disable(transmitter_a_->attributes());
  diff_host_receiver->Disable(transmitter_a_->attributes());

  transmitter_a_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(msgs.size(), 0);
}

TEST_F(HybridTransceiverTest,
       enable_and_disable_with_param_same_host_diff_proc_and_diff_host) {
  RoleAttributes same_host_attr;
  same_host_attr.set_host_name(common::GlobalData::Instance()->HostName());
  same_host_attr.set_host_ip(common::GlobalData::Instance()->HostIp());
  same_host_attr.set_process_id(common::GlobalData::Instance()->ProcessId());
  same_host_attr.mutable_qos_profile()->CopyFrom(
      QosProfileConf::QOS_PROFILE_DEFAULT);
  same_host_attr.set_channel_name(channel_name_);
  same_host_attr.set_channel_id(common::Hash(channel_name_));

  RoleAttributes diff_host_attr;
  diff_host_attr.set_host_name("remote_host");
  diff_host_attr.set_host_ip("10.255.255.1");
  diff_host_attr.set_process_id(12345);
  diff_host_attr.mutable_qos_profile()->CopyFrom(
      QosProfileConf::QOS_PROFILE_DEFAULT);
  diff_host_attr.set_channel_name(channel_name_);
  diff_host_attr.set_channel_id(common::Hash(channel_name_));

  std::mutex mtx;
  std::vector<proto::UnitTest> msgs;
  ReceiverPtr same_host_receiver =
      std::make_shared<HybridReceiver<proto::UnitTest>>(
          same_host_attr,
          [&](const std::shared_ptr<proto::UnitTest>& msg,
              const MessageInfo& msg_info, const RoleAttributes& attr) {
            (void)msg_info;
            (void)attr;
            std::lock_guard<std::mutex> lock(mtx);
            msgs.emplace_back(*msg);
          },
          Transport::Instance()->participant());

  ReceiverPtr diff_host_receiver =
      std::make_shared<HybridReceiver<proto::UnitTest>>(
          diff_host_attr,
          [&](const std::shared_ptr<proto::UnitTest>& msg,
              const MessageInfo& msg_info, const RoleAttributes& attr) {
            (void)msg_info;
            (void)attr;
            std::lock_guard<std::mutex> lock(mtx);
            msgs.emplace_back(*msg);
          },
          Transport::Instance()->participant());

  std::string class_name(
      "enable_and_disable_with_param_same_host_diff_proc_and_diff_host");
  auto msg = std::make_shared<proto::UnitTest>();
  msg->set_class_name(class_name);
  msg->set_case_name(class_name);

  transmitter_b_->Enable(same_host_receiver->attributes());
  transmitter_b_->Enable(diff_host_receiver->attributes());
  same_host_receiver->Enable(transmitter_b_->attributes());
  diff_host_receiver->Enable(transmitter_b_->attributes());

  transmitter_b_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(msgs.size(), 2);
  for (auto& item : msgs) {
    EXPECT_EQ(item.class_name(), class_name);
    EXPECT_EQ(item.case_name(), class_name);
  }

  msgs.clear();
  transmitter_b_->Disable(same_host_receiver->attributes());
  transmitter_b_->Disable(diff_host_receiver->attributes());
  same_host_receiver->Disable(transmitter_b_->attributes());
  diff_host_receiver->Disable(transmitter_b_->attributes());

  transmitter_b_->Transmit(msg);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(msgs.size(), 0);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  apollo::cyber::Init(argv[0]);
  apollo::cyber::transport::Transport::Instance();
  auto res = RUN_ALL_TESTS();
  apollo::cyber::Clear();
  return res;
}

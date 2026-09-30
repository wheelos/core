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

#ifndef CYBER_NODE_WRITER_H_
#define CYBER_NODE_WRITER_H_

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cyber/proto/topology_change.pb.h"

#include "cyber/common/log.h"
#include "cyber/metrics/metrics.h"
#include "cyber/node/writer_base.h"
#include "cyber/service_discovery/topology_manager.h"
#include "cyber/transport/transport.h"

namespace apollo {
namespace cyber {

class WriterTestPeer;

/**
 * @class Writer<MessageT>
 * @brief The Channel Writer has only one function: publish message through the
 * channel pointed in its RoleAttributes
 *
 * @tparam MessageT Message Type of the Writer handles
 */
template <typename MessageT>
class Writer : public WriterBase {
 public:
  using LoanedMessage = transport::LoanedMessage<MessageT>;
  using TransmitterPtr = std::shared_ptr<transport::Transmitter<MessageT>>;
  using ChangeConnection =
      typename service_discovery::Manager::ChangeConnection;
  using ChannelManagerPtr = service_discovery::ChannelManagerPtr;

  /**
   * @brief Construct a new Writer object
   *
   * @param role_attr we use RoleAttributes to identify a Writer
   */
  explicit Writer(const proto::RoleAttributes& role_attr);
  virtual ~Writer();

  /**
   * @brief Init the Writer
   *
   * @return true if init successfully
   * @return false if init failed
   */
  bool Init() override;

  /**
   * @brief Shutdown the Writer
   */
  void Shutdown() override;

  /**
   * @brief Write a MessageT instance
   *
   * Copies msg into a shared pointer before submitting it to the transport.
   * @param msg the message to write
   * @return the transport's synchronous submission result, not delivery or
   * callback completion. HYBRID returns true with no active readers or when
   * at least one active backend accepts the message.
   */
  virtual bool Write(const MessageT& msg);

  /**
   * @brief Write a shared ptr of MessageT
   *
   * The framework does not copy the message object here; transports may
   * serialize or copy its contents.
   * @param msg_ptr the message shared pointer to write
   * @return the transport's synchronous submission result, not delivery or
   * callback completion. HYBRID returns true with no active readers or when
   * at least one active backend accepts the message.
   */
  virtual bool Write(const std::shared_ptr<MessageT>& msg_ptr);
  virtual bool Loan(std::size_t size, LoanedMessage* loaned_msg);
  virtual bool Publish(LoanedMessage&& loaned_msg);

  /**
   * @brief Is there any Reader that subscribes our Channel?
   * You can publish message when this return true
   *
   * @return true if the channel has reader
   * @return false if the channel has no reader
   */
  bool HasReader() override;

  /**
   * @brief Get all Readers that subscriber our writing channel
   *
   * @param readers vector result of RoleAttributes
   */
  void GetReaders(std::vector<proto::RoleAttributes>* readers) override;

 private:
  TransmitterPtr SnapshotTransmitter(
      std::shared_ptr<metrics::Endpoint>* metric = nullptr);
  static void RecordPublishOutcome(
      const std::shared_ptr<metrics::Endpoint>& metric,
      metrics::TimePoint start, bool success);
  ChannelManagerPtr SnapshotChannelManager();
  void JoinTheTopology();
  void LeaveTheTopology();
  void OnChannelChange(const proto::ChangeMsg& change_msg);

  TransmitterPtr transmitter_;
  std::shared_ptr<metrics::Endpoint> metric_;

  ChangeConnection change_conn_;
  service_discovery::ChannelManagerPtr channel_manager_;

  friend class WriterTestPeer;
};

template <typename MessageT>
Writer<MessageT>::Writer(const proto::RoleAttributes& role_attr)
    : WriterBase(role_attr), transmitter_(nullptr), channel_manager_(nullptr) {}

template <typename MessageT>
Writer<MessageT>::~Writer() {
  Shutdown();
}

template <typename MessageT>
bool Writer<MessageT>::Init() {
  {
    std::lock_guard<std::mutex> g(lock_);
    if (init_) {
      return true;
    }
    transmitter_ =
        transport::Transport::Instance()->CreateTransmitter<MessageT>(
            role_attr_);
    if (transmitter_ == nullptr) {
      return false;
    }
    init_ = true;
    metric_ = metrics::Registry::Instance().RegisterWriter(
        role_attr_.channel_name(), role_attr_.node_name());
  }
  this->role_attr_.set_id(transmitter_->id().HashValue());
  channel_manager_ =
      service_discovery::TopologyManager::Instance()->channel_manager();
  JoinTheTopology();
  return true;
}

template <typename MessageT>
void Writer<MessageT>::Shutdown() {
  {
    std::lock_guard<std::mutex> g(lock_);
    if (!init_) {
      return;
    }
    init_ = false;
  }
  LeaveTheTopology();
  {
    std::lock_guard<std::mutex> g(lock_);
    transmitter_ = nullptr;
    channel_manager_ = nullptr;
    metric_.reset();
  }
}

template <typename MessageT>
typename Writer<MessageT>::TransmitterPtr
Writer<MessageT>::SnapshotTransmitter(
    std::shared_ptr<metrics::Endpoint>* metric) {
  std::lock_guard<std::mutex> g(lock_);
  if (metric != nullptr) {
    *metric = metric_;
  }
  if (!init_) {
    return nullptr;
  }
  return transmitter_;
}

template <typename MessageT>
typename Writer<MessageT>::ChannelManagerPtr
Writer<MessageT>::SnapshotChannelManager() {
  std::lock_guard<std::mutex> g(lock_);
  if (!init_) {
    return nullptr;
  }
  return channel_manager_;
}

template <typename MessageT>
void Writer<MessageT>::RecordPublishOutcome(
    const std::shared_ptr<metrics::Endpoint>& metric,
    metrics::TimePoint start, bool success) {
  if (!metric) return;
  const auto end = metrics::Clock::now();
  if (start == metrics::TimePoint{}) {
    metric->RecordPublish(success, end);
  } else {
    metric->RecordPublish(success, end - start, end);
  }
}

template <typename MessageT>
bool Writer<MessageT>::Write(const MessageT& msg) {
  const bool metrics_enabled =
      metrics::Registry::Instance().mode() != metrics::Mode::Off;
  const auto start = metrics_enabled ? metrics::Clock::now()
                                    : metrics::TimePoint{};
  std::shared_ptr<metrics::Endpoint> metric;
  auto transmitter =
      SnapshotTransmitter(metrics_enabled ? &metric : nullptr);
  bool result = false;
  if (transmitter != nullptr) {
    auto msg_ptr = std::make_shared<MessageT>(msg);
    result = transmitter->Transmit(msg_ptr);
  }
  RecordPublishOutcome(metric, start, result);
  return result;
}

template <typename MessageT>
bool Writer<MessageT>::Write(const std::shared_ptr<MessageT>& msg_ptr) {
  const bool metrics_enabled =
      metrics::Registry::Instance().mode() != metrics::Mode::Off;
  const auto start = metrics_enabled ? metrics::Clock::now()
                                    : metrics::TimePoint{};
  std::shared_ptr<metrics::Endpoint> metric;
  auto transmitter =
      SnapshotTransmitter(metrics_enabled ? &metric : nullptr);
  const bool result = transmitter != nullptr && transmitter->Transmit(msg_ptr);
  RecordPublishOutcome(metric, start, result);
  return result;
}

template <typename MessageT>
bool Writer<MessageT>::Loan(std::size_t size, LoanedMessage* loaned_msg) {
  auto transmitter = SnapshotTransmitter();
  RETURN_VAL_IF(transmitter == nullptr, false);
  return transmitter->Loan(size, loaned_msg);
}

template <typename MessageT>
bool Writer<MessageT>::Publish(LoanedMessage&& loaned_msg) {
  const bool metrics_enabled =
      metrics::Registry::Instance().mode() != metrics::Mode::Off;
  const auto start = metrics_enabled ? metrics::Clock::now()
                                    : metrics::TimePoint{};
  std::shared_ptr<metrics::Endpoint> metric;
  auto transmitter =
      SnapshotTransmitter(metrics_enabled ? &metric : nullptr);
  const bool result = transmitter != nullptr &&
                      transmitter->Publish(std::move(loaned_msg));
  RecordPublishOutcome(metric, start, result);
  return result;
}

template <typename MessageT>
void Writer<MessageT>::JoinTheTopology() {
  // add listener
  change_conn_ = channel_manager_->AddChangeListener(std::bind(
      &Writer<MessageT>::OnChannelChange, this, std::placeholders::_1));

  // get peer readers
  const std::string& channel_name = this->role_attr_.channel_name();
  std::vector<proto::RoleAttributes> readers;
  channel_manager_->GetReadersOfChannel(channel_name, &readers);
  for (auto& reader : readers) {
    transmitter_->Enable(reader);
  }

  channel_manager_->Join(this->role_attr_, proto::RoleType::ROLE_WRITER,
                         message::HasSerializer<MessageT>::value);
}

template <typename MessageT>
void Writer<MessageT>::LeaveTheTopology() {
  channel_manager_->RemoveChangeListener(change_conn_);
  channel_manager_->Leave(this->role_attr_, proto::RoleType::ROLE_WRITER);
}

template <typename MessageT>
void Writer<MessageT>::OnChannelChange(const proto::ChangeMsg& change_msg) {
  if (change_msg.role_type() != proto::RoleType::ROLE_READER) {
    return;
  }

  auto& reader_attr = change_msg.role_attr();
  if (reader_attr.channel_name() != this->role_attr_.channel_name()) {
    return;
  }

  auto operate_type = change_msg.operate_type();
  if (operate_type == proto::OperateType::OPT_JOIN) {
    transmitter_->Enable(reader_attr);
  } else {
    transmitter_->Disable(reader_attr);
  }
}

template <typename MessageT>
bool Writer<MessageT>::HasReader() {
  auto channel_manager = SnapshotChannelManager();
  RETURN_VAL_IF(channel_manager == nullptr, false);
  return channel_manager->HasReader(role_attr_.channel_name());
}

template <typename MessageT>
void Writer<MessageT>::GetReaders(std::vector<proto::RoleAttributes>* readers) {
  if (readers == nullptr) {
    return;
  }

  auto channel_manager = SnapshotChannelManager();
  RETURN_IF(channel_manager == nullptr);
  channel_manager->GetReadersOfChannel(role_attr_.channel_name(), readers);
}

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_NODE_WRITER_H_

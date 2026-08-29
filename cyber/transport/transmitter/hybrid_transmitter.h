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

#ifndef CYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_
#define CYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "cyber/common/global_data.h"
#include "cyber/common/log.h"
#include "cyber/common/types.h"
#include "cyber/proto/role_attributes.pb.h"
#include "cyber/proto/transport_conf.pb.h"
#include "cyber/task/task.h"
#include "cyber/transport/message/history.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/rtps/participant.h"
#include "cyber/transport/transmitter/intra_transmitter.h"
#include "cyber/transport/transmitter/iceoryx_transmitter.h"
#include "cyber/transport/transmitter/rtps_transmitter.h"
#include "cyber/transport/transmitter/shm_transmitter.h"
#include "cyber/transport/transmitter/transmitter.h"

namespace apollo {
namespace cyber {
namespace transport {

using apollo::cyber::proto::OptionalMode;
using apollo::cyber::proto::QosDurabilityPolicy;
using apollo::cyber::proto::RoleAttributes;

class HybridTransmitterTestPeer;

namespace {

inline void NormalizeHybridTransmitterCommunicationMode(
    proto::CommunicationMode* mode) {
  if (mode == nullptr) {
    return;
  }
  if (mode->same_proc() != OptionalMode::INTRA) {
    AERROR << "invalid same_proc transport mode "
           << static_cast<int>(mode->same_proc())
           << ", forcing INTRA for same-process delivery";
    mode->set_same_proc(OptionalMode::INTRA);
  }
  if (mode->diff_proc() == OptionalMode::INTRA) {
    AERROR << "invalid diff_proc transport mode INTRA, forcing ICEORYX";
    mode->set_diff_proc(OptionalMode::ICEORYX);
  }
  if (mode->diff_host() != OptionalMode::RTPS) {
    AERROR << "invalid diff_host transport mode "
           << static_cast<int>(mode->diff_host())
           << ", forcing RTPS for cross-host delivery";
    mode->set_diff_host(OptionalMode::RTPS);
  }
}

template <typename M>
inline OptionalMode ResolveHybridTransmitterDiffProcMode(OptionalMode mode) {
  // Keep hybrid diff-proc zero-copy on the explicit Pod contract only.
  if (mode == OptionalMode::ICEORYX && !std::is_same<M, PodMessage>::value) {
    return OptionalMode::RTPS;
  }
  return mode;
}

}  // namespace

template <typename M>
class HybridTransmitter : public Transmitter<M> {
 public:
  using MessagePtr = std::shared_ptr<M>;
  using HistoryPtr = std::shared_ptr<History<M>>;
  using TransmitterPtr = std::shared_ptr<Transmitter<M>>;
  using TransmitterMap =
      std::unordered_map<OptionalMode, TransmitterPtr, std::hash<int>>;
  using ReceiverMap =
      std::unordered_map<OptionalMode, std::set<uint64_t>, std::hash<int>>;
  using CommunicationModePtr = std::shared_ptr<proto::CommunicationMode>;
  using MappingTable =
      std::unordered_map<Relation, OptionalMode, std::hash<int>>;

  HybridTransmitter(const RoleAttributes& attr,
                    const ParticipantPtr& participant);
  virtual ~HybridTransmitter();

  void Enable() override;
  void Disable() override;
  void Enable(const RoleAttributes& opposite_attr) override;
  void Disable(const RoleAttributes& opposite_attr) override;

  bool Transmit(const MessagePtr& msg, const MessageInfo& msg_info) override;
  bool IsLoanSupported() const override;
  bool Loan(std::size_t size, LoanedMessage<M>* loaned_msg) override;
  bool Publish(LoanedMessage<M>&& loaned_msg) override;

 private:
  bool HasActiveReceiversOnly(OptionalMode mode) const;
  void InitMode();
  void ObtainConfig();
  void InitHistory();
  void InitTransmitters();
  void ClearTransmitters();
  void InitReceivers();
  void ClearReceivers();
  void TransmitHistoryMsg(const RoleAttributes& opposite_attr);
  static void ReplayHistoryMsg(
      const RoleAttributes& transmitter_attr, const ParticipantPtr& participant,
      const RoleAttributes& opposite_attr,
      const std::vector<typename History<M>::CachedMessage>& msgs);
  Relation GetRelation(const RoleAttributes& opposite_attr);

  HistoryPtr history_;
  TransmitterMap transmitters_;
  ReceiverMap receivers_;
  std::mutex mutex_;
  std::mutex enable_mutex_;

  CommunicationModePtr mode_;
  MappingTable mapping_table_;

  ParticipantPtr participant_;

  friend class HybridTransmitterTestPeer;
};

template <typename M>
HybridTransmitter<M>::HybridTransmitter(const RoleAttributes& attr,
                                        const ParticipantPtr& participant)
    : Transmitter<M>(attr),
      history_(nullptr),
      mode_(nullptr),
      participant_(participant) {
  InitMode();
  ObtainConfig();
  InitHistory();
  InitTransmitters();
  InitReceivers();
}

template <typename M>
HybridTransmitter<M>::~HybridTransmitter() {
  ClearReceivers();
  ClearTransmitters();
}

template <typename M>
void HybridTransmitter<M>::Enable() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : transmitters_) {
    item.second->Enable();
  }
}

template <typename M>
void HybridTransmitter<M>::Disable() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : transmitters_) {
    item.second->Disable();
  }
}

template <typename M>
void HybridTransmitter<M>::Enable(const RoleAttributes& opposite_attr) {
  auto relation = GetRelation(opposite_attr);
  if (relation == NO_RELATION) {
    return;
  }

  uint64_t id = opposite_attr.id();
  auto mode = mapping_table_[relation];
  TransmitterPtr target_transmitter = nullptr;
  bool receiver_inserted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target_transmitter = transmitters_[mode];
  }

  if (target_transmitter != nullptr) {
    {
      std::lock_guard<std::mutex> enable_lock(enable_mutex_);
      bool is_first_receiver = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        is_first_receiver = receivers_[mode].empty();
      }
      if (is_first_receiver) {
        target_transmitter->Enable();
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        receiver_inserted = receivers_[mode].insert(id).second;
      }
    }
    if (receiver_inserted) {
      TransmitHistoryMsg(opposite_attr);
    }
  }
}

template <typename M>
void HybridTransmitter<M>::Disable(const RoleAttributes& opposite_attr) {
  auto relation = GetRelation(opposite_attr);
  if (relation == NO_RELATION) {
    return;
  }

  uint64_t id = opposite_attr.id();
  std::lock_guard<std::mutex> enable_lock(enable_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  receivers_[mapping_table_[relation]].erase(id);
}

template <typename M>
bool HybridTransmitter<M>::Transmit(const MessagePtr& msg,
                                    const MessageInfo& msg_info) {
  std::vector<std::pair<OptionalMode, TransmitterPtr>> active_transmitters;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    history_->Add(msg, msg_info);
    for (auto& item : transmitters_) {
      const auto mode = item.first;
      const auto receiver_it = receivers_.find(mode);
      if (receiver_it != receivers_.end() && !receiver_it->second.empty()) {
        active_transmitters.emplace_back(mode, item.second);
      }
    }
  }

  if (active_transmitters.empty()) {
    return true;
  }

  bool delivered = false;
  for (auto& [mode, transmitter] : active_transmitters) {
    if (transmitter->Transmit(msg, msg_info)) {
      delivered = true;
    } else {
      AERROR << "hybrid transmit failed: channel=" << this->attr_.channel_name()
             << " mode=" << static_cast<int>(mode);
    }
  }
  return delivered;
}

template <typename M>
bool HybridTransmitter<M>::IsLoanSupported() const {
  auto it = transmitters_.find(OptionalMode::ICEORYX);
  return it != transmitters_.end() && it->second != nullptr &&
         it->second->IsLoanSupported();
}

template <typename M>
bool HybridTransmitter<M>::HasActiveReceiversOnly(OptionalMode mode) const {
  bool has_target = false;
  for (const auto& item : receivers_) {
    if (item.second.empty()) {
      continue;
    }
    if (item.first != mode) {
      return false;
    }
    has_target = true;
  }
  return has_target;
}

template <typename M>
bool HybridTransmitter<M>::Loan(std::size_t size, LoanedMessage<M>* loaned_msg) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = transmitters_.find(OptionalMode::ICEORYX);
  if (it != transmitters_.end() && it->second != nullptr &&
      HasActiveReceiversOnly(OptionalMode::ICEORYX)) {
    return it->second->Loan(size, loaned_msg);
  }
  it = transmitters_.find(OptionalMode::SHM);
  if (it != transmitters_.end() && it->second != nullptr &&
      HasActiveReceiversOnly(OptionalMode::SHM)) {
    return it->second->Loan(size, loaned_msg);
  }
  return false;
}

template <typename M>
bool HybridTransmitter<M>::Publish(LoanedMessage<M>&& loaned_msg) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = transmitters_.find(OptionalMode::ICEORYX);
  if (it != transmitters_.end() && it->second != nullptr &&
      HasActiveReceiversOnly(OptionalMode::ICEORYX)) {
    return it->second->Publish(std::move(loaned_msg));
  }
  it = transmitters_.find(OptionalMode::SHM);
  if (it != transmitters_.end() && it->second != nullptr &&
      HasActiveReceiversOnly(OptionalMode::SHM)) {
    return it->second->Publish(std::move(loaned_msg));
  }
  return false;
}

template <typename M>
void HybridTransmitter<M>::InitMode() {
  mode_ = std::make_shared<proto::CommunicationMode>();
  NormalizeHybridTransmitterCommunicationMode(mode_.get());
  mapping_table_[SAME_PROC] = mode_->same_proc();
  mapping_table_[DIFF_PROC] =
      ResolveHybridTransmitterDiffProcMode<M>(mode_->diff_proc());
  mapping_table_[DIFF_HOST] = mode_->diff_host();
}

template <typename M>
void HybridTransmitter<M>::ObtainConfig() {
  auto& global_conf = common::GlobalData::Instance()->Config();
  if (!global_conf.has_transport_conf()) {
    return;
  }
  if (!global_conf.transport_conf().has_communication_mode()) {
    return;
  }
  mode_->CopyFrom(global_conf.transport_conf().communication_mode());
  NormalizeHybridTransmitterCommunicationMode(mode_.get());

  mapping_table_[SAME_PROC] = mode_->same_proc();
  mapping_table_[DIFF_PROC] =
      ResolveHybridTransmitterDiffProcMode<M>(mode_->diff_proc());
  mapping_table_[DIFF_HOST] = mode_->diff_host();
}

template <typename M>
void HybridTransmitter<M>::InitHistory() {
  HistoryAttributes history_attr(this->attr_.qos_profile().history(),
                                 this->attr_.qos_profile().depth());
  history_ = std::make_shared<History<M>>(history_attr);
  if (this->attr_.qos_profile().durability() ==
      QosDurabilityPolicy::DURABILITY_TRANSIENT_LOCAL) {
    history_->Enable();
  }
}

template <typename M>
void HybridTransmitter<M>::InitTransmitters() {
  std::set<OptionalMode> modes;
  modes.insert(mapping_table_[SAME_PROC]);
  modes.insert(mapping_table_[DIFF_PROC]);
  modes.insert(mapping_table_[DIFF_HOST]);
  for (auto& mode : modes) {
    switch (mode) {
      case OptionalMode::INTRA:
        transmitters_[mode] =
            std::make_shared<IntraTransmitter<M>>(this->attr_);
        break;
      case OptionalMode::SHM:
        transmitters_[mode] = std::make_shared<ShmTransmitter<M>>(this->attr_);
        break;
      case OptionalMode::ICEORYX:
        transmitters_[mode] =
            std::make_shared<IceoryxTransmitter<M>>(this->attr_);
        break;
      default:
        transmitters_[mode] =
            std::make_shared<RtpsTransmitter<M>>(this->attr_, participant_);
        break;
    }
  }
}

template <typename M>
void HybridTransmitter<M>::ClearTransmitters() {
  for (auto& item : transmitters_) {
    item.second->Disable();
  }
  transmitters_.clear();
}

template <typename M>
void HybridTransmitter<M>::InitReceivers() {
  std::set<uint64_t> empty;
  for (auto& item : transmitters_) {
    receivers_[item.first] = empty;
  }
}

template <typename M>
void HybridTransmitter<M>::ClearReceivers() {
  receivers_.clear();
}

template <typename M>
void HybridTransmitter<M>::TransmitHistoryMsg(
    const RoleAttributes& opposite_attr) {
  // check qos
  if (this->attr_.qos_profile().durability() !=
      QosDurabilityPolicy::DURABILITY_TRANSIENT_LOCAL) {
    return;
  }

  // get unsent messages
  std::vector<typename History<M>::CachedMessage> unsent_msgs;
  history_->GetCachedMessage(&unsent_msgs);
  if (unsent_msgs.empty()) {
    return;
  }

  auto transmitter_attr = this->attr_;
  auto participant = participant_;
  auto receiver_attr = opposite_attr;
  auto replay = cyber::Async([transmitter_attr, participant, receiver_attr,
                              msgs = std::move(unsent_msgs)]() {
    ReplayHistoryMsg(transmitter_attr, participant, receiver_attr, msgs);
  });
  if (!replay.valid()) {
    AERROR << "failed to enqueue history replay: channel="
           << this->attr_.channel_name()
           << " receiver_id=" << opposite_attr.id();
  }
}

template <typename M>
void HybridTransmitter<M>::ReplayHistoryMsg(
    const RoleAttributes& transmitter_attr, const ParticipantPtr& participant,
    const RoleAttributes& opposite_attr,
    const std::vector<typename History<M>::CachedMessage>& msgs) {
  RoleAttributes new_attr;
  new_attr.CopyFrom(transmitter_attr);
  std::string new_channel_name = std::to_string(transmitter_attr.id()) +
                                 std::to_string(opposite_attr.id());
  uint64_t channel_id = common::GlobalData::RegisterChannel(new_channel_name);
  new_attr.set_channel_name(new_channel_name);
  new_attr.set_channel_id(channel_id);
  auto new_transmitter =
      std::make_shared<RtpsTransmitter<M>>(new_attr, participant);
  new_transmitter->Enable();

  for (auto& item : msgs) {
    new_transmitter->Transmit(item.msg, item.msg_info);
    cyber::USleep(1000);
  }
  new_transmitter->Disable();
  ADEBUG << "history replay exit.";
}

template <typename M>
Relation HybridTransmitter<M>::GetRelation(
    const RoleAttributes& opposite_attr) {
  if (opposite_attr.channel_name() != this->attr_.channel_name()) {
    return NO_RELATION;
  }
  if (opposite_attr.host_ip() != this->attr_.host_ip()) {
    return DIFF_HOST;
  }
  if (opposite_attr.process_id() != this->attr_.process_id()) {
    return DIFF_PROC;
  }

  return SAME_PROC;
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_

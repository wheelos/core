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

#ifndef CYBER_TRANSPORT_RECEIVER_HYBRID_RECEIVER_H_
#define CYBER_TRANSPORT_RECEIVER_HYBRID_RECEIVER_H_

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/common/global_data.h"
#include "cyber/common/log.h"
#include "cyber/common/types.h"
#include "cyber/proto/role_attributes.pb.h"
#include "cyber/service_discovery/role/role.h"
#include "cyber/task/task.h"
#include "cyber/time/time.h"
#include "cyber/transport/message/pod_message.h"
#include "cyber/transport/receiver/iceoryx_receiver.h"
#include "cyber/transport/receiver/intra_receiver.h"
#include "cyber/transport/receiver/rtps_receiver.h"
#include "cyber/transport/receiver/shm_receiver.h"
#include "cyber/transport/rtps/participant.h"

namespace apollo {
namespace cyber {
namespace transport {

using apollo::cyber::proto::OptionalMode;
using apollo::cyber::proto::QosDurabilityPolicy;
using apollo::cyber::proto::RoleAttributes;

namespace {

inline void NormalizeHybridReceiverCommunicationMode(
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
inline OptionalMode ResolveHybridReceiverDiffProcMode(OptionalMode mode) {
  // Keep hybrid diff-proc zero-copy on the explicit Pod contract only.
  if (mode == OptionalMode::ICEORYX && !std::is_same<M, PodMessage>::value) {
    return OptionalMode::RTPS;
  }
  return mode;
}

}  // namespace

template <typename M>
class HybridReceiver : public Receiver<M> {
 public:
  using HistoryPtr = std::shared_ptr<History<M>>;
  using ReceiverPtr = std::shared_ptr<Receiver<M>>;
  using ReceiverContainer =
      std::unordered_map<OptionalMode, ReceiverPtr, std::hash<int>>;
  using TransmitterContainer =
      std::unordered_map<OptionalMode,
                         std::unordered_map<uint64_t, RoleAttributes>,
                         std::hash<int>>;
  using CommunicationModePtr = std::shared_ptr<proto::CommunicationMode>;
  using MappingTable =
      std::unordered_map<Relation, OptionalMode, std::hash<int>>;

  HybridReceiver(const RoleAttributes& attr,
                 const typename Receiver<M>::MessageListener& msg_listener,
                 const ParticipantPtr& participant);
  virtual ~HybridReceiver();

  void Enable() override;
  void Disable() override;

  void Enable(const RoleAttributes& opposite_attr) override;
  void Disable(const RoleAttributes& opposite_attr) override;

 private:
  void InitMode();
  void ObtainConfig();
  void InitHistory();
  void InitReceivers();
  void ClearReceivers();
  void InitTransmitters();
  void ClearTransmitters();
  void ReceiveHistoryMsg(const RoleAttributes& opposite_attr);
  static void ThreadFunc(
      const RoleAttributes& opposite_attr,
      const RoleAttributes& receiver_attr,
      const typename Receiver<M>::MessageListener& message_listener);
  Relation GetRelation(const RoleAttributes& opposite_attr);

  HistoryPtr history_;
  ReceiverContainer receivers_;
  TransmitterContainer transmitters_;
  std::mutex mutex_;

  CommunicationModePtr mode_;
  MappingTable mapping_table_;

  ParticipantPtr participant_;
};

template <typename M>
HybridReceiver<M>::HybridReceiver(
    const RoleAttributes& attr,
    const typename Receiver<M>::MessageListener& msg_listener,
    const ParticipantPtr& participant)
    : Receiver<M>(attr, msg_listener),
      history_(nullptr),
      participant_(participant) {
  InitMode();
  ObtainConfig();
  InitHistory();
  InitReceivers();
  InitTransmitters();
}

template <typename M>
HybridReceiver<M>::~HybridReceiver() {
  ClearTransmitters();
  ClearReceivers();
}

template <typename M>
void HybridReceiver<M>::Enable() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : receivers_) {
    item.second->Enable();
  }
}

template <typename M>
void HybridReceiver<M>::Disable() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : receivers_) {
    item.second->Disable();
  }
}

template <typename M>
void HybridReceiver<M>::Enable(const RoleAttributes& opposite_attr) {
  auto relation = GetRelation(opposite_attr);
  RETURN_IF(relation == NO_RELATION);

  uint64_t id = opposite_attr.id();
  std::lock_guard<std::mutex> lock(mutex_);
  if (transmitters_[mapping_table_[relation]].count(id) == 0) {
    transmitters_[mapping_table_[relation]].insert(
        std::make_pair(id, opposite_attr));
    receivers_[mapping_table_[relation]]->Enable(opposite_attr);
    ReceiveHistoryMsg(opposite_attr);
  }
}

template <typename M>
void HybridReceiver<M>::Disable(const RoleAttributes& opposite_attr) {
  auto relation = GetRelation(opposite_attr);
  RETURN_IF(relation == NO_RELATION);

  uint64_t id = opposite_attr.id();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto mode = mapping_table_[relation];
  auto& mode_transmitters = transmitters_[mode];
  if (mode_transmitters.count(id) > 0) {
    mode_transmitters.erase(id);
    // Dispatcher-backed receivers filter by writer; ICEORYX shares one worker.
    if (mode != OptionalMode::ICEORYX || mode_transmitters.empty()) {
      receivers_[mode]->Disable(opposite_attr);
    }
  }
}

template <typename M>
void HybridReceiver<M>::InitMode() {
  mode_ = std::make_shared<proto::CommunicationMode>();
  NormalizeHybridReceiverCommunicationMode(mode_.get());
  mapping_table_[SAME_PROC] = mode_->same_proc();
  mapping_table_[DIFF_PROC] =
      ResolveHybridReceiverDiffProcMode<M>(mode_->diff_proc());
  mapping_table_[DIFF_HOST] = mode_->diff_host();
}

template <typename M>
void HybridReceiver<M>::ObtainConfig() {
  auto& global_conf = common::GlobalData::Instance()->Config();
  if (!global_conf.has_transport_conf()) {
    return;
  }
  if (!global_conf.transport_conf().has_communication_mode()) {
    return;
  }
  mode_->CopyFrom(global_conf.transport_conf().communication_mode());
  NormalizeHybridReceiverCommunicationMode(mode_.get());

  mapping_table_[SAME_PROC] = mode_->same_proc();
  mapping_table_[DIFF_PROC] =
      ResolveHybridReceiverDiffProcMode<M>(mode_->diff_proc());
  mapping_table_[DIFF_HOST] = mode_->diff_host();
}

template <typename M>
void HybridReceiver<M>::InitHistory() {
  HistoryAttributes history_attr(this->attr_.qos_profile().history(),
                                 this->attr_.qos_profile().depth());
  history_ = std::make_shared<History<M>>(history_attr);
  if (this->attr_.qos_profile().durability() ==
      QosDurabilityPolicy::DURABILITY_TRANSIENT_LOCAL) {
    history_->Enable();
  }
}

template <typename M>
void HybridReceiver<M>::InitReceivers() {
  std::set<OptionalMode> modes;
  modes.insert(mapping_table_[SAME_PROC]);
  modes.insert(mapping_table_[DIFF_PROC]);
  modes.insert(mapping_table_[DIFF_HOST]);
  auto listener = std::bind(&HybridReceiver<M>::OnNewMessage, this,
                            std::placeholders::_1, std::placeholders::_2);
  for (auto& mode : modes) {
    switch (mode) {
      case OptionalMode::INTRA:
        receivers_[mode] =
            std::make_shared<IntraReceiver<M>>(this->attr_, listener);
        break;
      case OptionalMode::SHM:
        receivers_[mode] =
            std::make_shared<ShmReceiver<M>>(this->attr_, listener);
        break;
      case OptionalMode::ICEORYX:
        receivers_[mode] =
            std::make_shared<IceoryxReceiver<M>>(this->attr_, listener);
        break;
      default:
        receivers_[mode] =
            std::make_shared<RtpsReceiver<M>>(this->attr_, listener);
        break;
    }
  }
}

template <typename M>
void HybridReceiver<M>::ClearReceivers() {
  receivers_.clear();
}

template <typename M>
void HybridReceiver<M>::InitTransmitters() {
  std::unordered_map<uint64_t, RoleAttributes> empty;
  for (auto& item : receivers_) {
    transmitters_[item.first] = empty;
  }
}

template <typename M>
void HybridReceiver<M>::ClearTransmitters() {
  for (auto& item : transmitters_) {
    for (auto& upper_reach : item.second) {
      receivers_[item.first]->Disable(upper_reach.second);
    }
  }
  transmitters_.clear();
}

template <typename M>
void HybridReceiver<M>::ReceiveHistoryMsg(const RoleAttributes& opposite_attr) {
  // check qos
  if (opposite_attr.qos_profile().durability() !=
      QosDurabilityPolicy::DURABILITY_TRANSIENT_LOCAL) {
    return;
  }

  auto opposite = opposite_attr;
  auto receiver = this->attr_;
  auto message_listener = this->msg_listener_;
  auto replay = cyber::Async(&HybridReceiver<M>::ThreadFunc, opposite, receiver,
                             message_listener);
  if (!replay.valid()) {
    AERROR << "failed to enqueue history receive: channel="
           << this->attr_.channel_name()
           << " transmitter_id=" << opposite_attr.id();
  }
}

template <typename M>
void HybridReceiver<M>::ThreadFunc(
    const RoleAttributes& opposite_attr, const RoleAttributes& receiver_attr,
    const typename Receiver<M>::MessageListener& message_listener) {
  std::string channel_name =
      std::to_string(opposite_attr.id()) + std::to_string(receiver_attr.id());
  uint64_t channel_id = common::GlobalData::RegisterChannel(channel_name);

  RoleAttributes attr(receiver_attr);
  attr.set_channel_name(channel_name);
  attr.set_channel_id(channel_id);
  attr.mutable_qos_profile()->CopyFrom(opposite_attr.qos_profile());

  std::atomic<bool> is_msg_arrived(false);
  auto listener = [message_listener, receiver_attr, &is_msg_arrived](
                      const std::shared_ptr<M>& msg,
                      const MessageInfo& msg_info, const RoleAttributes&) {
    is_msg_arrived.store(true);
    if (message_listener != nullptr) {
      message_listener(msg, msg_info, receiver_attr);
    }
  };

  auto receiver = std::make_shared<RtpsReceiver<M>>(attr, listener);
  receiver->Enable();

  do {
    is_msg_arrived.store(false);
    cyber::USleep(1000000);
  } while (is_msg_arrived.load());

  receiver->Disable();
  ADEBUG << "recv threadfunc exit.";
}

template <typename M>
Relation HybridReceiver<M>::GetRelation(const RoleAttributes& opposite_attr) {
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

#endif  // CYBER_TRANSPORT_RECEIVER_HYBRID_RECEIVER_H_

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

#include "cyber/transport/rtps/participant.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <limits>

#include "fastrtps/rtps/resources/ResourceEvent.h"

#include "cyber/proto/transport_conf.pb.h"

#include "cyber/common/global_data.h"
#include "cyber/common/log.h"
#include "cyber/state.h"

namespace apollo {
namespace cyber {
namespace transport {

namespace {

class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  int Release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  int fd_;
};

}  // namespace

Participant::Participant(const std::string& name, int send_port,
                         eprosima::fastrtps::ParticipantListener* listener)
    : shutdown_(false),
      name_(name),
      send_port_(send_port),
      listener_(listener),
      fastrtps_participant_(nullptr),
      participant_lock_fd_(-1) {}

Participant::~Participant() { Shutdown(); }

void Participant::StopEventThread() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (fastrtps_participant_ != nullptr) {
    fastrtps_participant_->get_resource_event().stop_thread();
  }
}

void Participant::Shutdown() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (shutdown_.exchange(true)) {
    return;
  }
  const bool retain_native_participant =
      listener_ == nullptr && apollo::cyber::IsShutdown();
  if (fastrtps_participant_ != nullptr) {
    fastrtps_participant_->get_resource_event().stop_thread();
    if (!retain_native_participant) {
      eprosima::fastrtps::Domain::removeParticipant(fastrtps_participant_);
    }
    fastrtps_participant_ = nullptr;
    listener_ = nullptr;
  }
  // A retained participant still owns its RTPS ports, so intentionally keep
  // its slot lock descriptor open until the process exits.
  if (!retain_native_participant && participant_lock_fd_ >= 0) {
    close(participant_lock_fd_);
    participant_lock_fd_ = -1;
  }
}

eprosima::fastrtps::Participant* Participant::fastrtps_participant() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (shutdown_.load()) {
    return nullptr;
  }
  if (fastrtps_participant_ != nullptr) {
    return fastrtps_participant_;
  }

  CreateFastRtpsParticipant(name_, send_port_, listener_);
  return fastrtps_participant_;
}

void Participant::CreateFastRtpsParticipant(
    const std::string& name, int send_port,
    eprosima::fastrtps::ParticipantListener* listener) {
  uint32_t domain_id = 80;

  const char* val = ::getenv("CYBER_DOMAIN_ID");
  if (val != nullptr) {
    try {
      domain_id = std::stoi(val);
    } catch (const std::exception& e) {
      AERROR << "convert domain_id error " << e.what();
      return;
    }
  }

  auto part_attr_conf = std::make_shared<proto::RtpsParticipantAttr>();
  auto& global_conf = common::GlobalData::Instance()->Config();
  if (global_conf.has_transport_conf() &&
      global_conf.transport_conf().has_participant_attr()) {
    part_attr_conf->CopyFrom(global_conf.transport_conf().participant_attr());
  }

  eprosima::fastrtps::ParticipantAttributes attr;
  attr.rtps.port.domainIDGain =
      static_cast<uint16_t>(part_attr_conf->domain_id_gain());
  attr.rtps.port.portBase = static_cast<uint16_t>(part_attr_conf->port_base());
  attr.rtps.builtin.avoid_builtin_multicast = true;
  attr.rtps.builtin.discovery_config.use_SIMPLE_EndpointDiscoveryProtocol =
      true;
  attr.rtps.builtin.discovery_config.m_simpleEDP
      .use_PublicationReaderANDSubscriptionWriter = true;
  attr.rtps.builtin.discovery_config.m_simpleEDP
      .use_PublicationWriterANDSubscriptionReader = true;
  attr.domainId = domain_id;

  /**
   * The user should set the lease_duration and the announcement_period with
   * values that differ in at least 30%. Values too close to each other may
   * cause the failure of the writer liveliness assertion in networks with high
   * latency or with lots of communication errors.
   */
  attr.rtps.builtin.discovery_config.leaseDuration.seconds =
      part_attr_conf->lease_duration();
  attr.rtps.builtin.discovery_config.leaseDuration_announcementperiod.seconds =
      part_attr_conf->announcement_period();

  attr.rtps.setName(name.c_str());

  std::string ip_env("127.0.0.1");
  const char* ip_val = ::getenv("CYBER_IP");
  if (ip_val != nullptr) {
    ip_env = ip_val;
    if (ip_env.empty()) {
      AERROR << "invalid CYBER_IP (an empty string)";
      return;
    }
  }
  ADEBUG << "cyber ip: " << ip_env;

  RETURN_IF(send_port <= 0);
  const auto process_id = common::GlobalData::Instance()->ProcessId();
  constexpr uint32_t kParticipantIdRange = 128;
  const uint32_t participant_id_parity = (send_port == 11512) ? 1u : 0u;
  for (uint32_t id = participant_id_parity; id < (kParticipantIdRange * 2);
       id += 2) {
    eprosima::fastrtps::rtps::Locator_t peer_locator;
    peer_locator.kind = LOCATOR_KIND_UDPv4;
    RETURN_IF(
        !eprosima::fastrtps::rtps::IPLocator::setIPv4(peer_locator, ip_env));
    RETURN_IF(!eprosima::fastrtps::rtps::IPLocator::setPhysicalPort(
        peer_locator,
        static_cast<uint16_t>(attr.rtps.port.getUnicastPort(domain_id, id))));
    attr.rtps.builtin.initialPeersList.push_back(peer_locator);
  }

  const uint32_t start_slot = process_id % kParticipantIdRange;
  for (uint32_t attempt = 0; attempt < kParticipantIdRange; ++attempt) {
    const uint32_t slot = (start_slot + attempt) % kParticipantIdRange;
    const uint32_t participant_id = slot * 2 + participant_id_parity;
    const std::string lock_path = "/tmp/cyber_rtps_participant_" +
                                  std::to_string(domain_id) + "_" +
                                  std::to_string(participant_id) + ".lock";
    const int lock_fd =
        open(lock_path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
    if (lock_fd < 0) {
      continue;
    }
    ScopedFd scoped_lock_fd(lock_fd);
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
      continue;
    }
    attr.rtps.participantID = static_cast<int32_t>(participant_id);
    attr.rtps.builtin.metatrafficUnicastLocatorList.clear();
    attr.rtps.defaultUnicastLocatorList.clear();

    eprosima::fastrtps::rtps::Locator_t metatraffic_locator;
    metatraffic_locator.kind = LOCATOR_KIND_UDPv4;
    RETURN_IF(!eprosima::fastrtps::rtps::IPLocator::setIPv4(metatraffic_locator,
                                                            ip_env));
    RETURN_IF(!eprosima::fastrtps::rtps::IPLocator::setPhysicalPort(
        metatraffic_locator,
        static_cast<uint16_t>(
            attr.rtps.port.getUnicastPort(domain_id, participant_id))));
    attr.rtps.builtin.metatrafficUnicastLocatorList.push_back(
        metatraffic_locator);

    const uint32_t user_unicast_port =
        attr.rtps.port.portBase + attr.rtps.port.domainIDGain * domain_id +
        attr.rtps.port.offsetd3 +
        attr.rtps.port.participantIDGain * participant_id;
    RETURN_IF(user_unicast_port > std::numeric_limits<uint16_t>::max());
    eprosima::fastrtps::rtps::Locator_t user_locator;
    user_locator.kind = LOCATOR_KIND_UDPv4;
    RETURN_IF(
        !eprosima::fastrtps::rtps::IPLocator::setIPv4(user_locator, ip_env));
    RETURN_IF(!eprosima::fastrtps::rtps::IPLocator::setPhysicalPort(
        user_locator, static_cast<uint16_t>(user_unicast_port)));
    attr.rtps.defaultUnicastLocatorList.push_back(user_locator);

    fastrtps_participant_ =
        eprosima::fastrtps::Domain::createParticipant(attr, listener);
    if (fastrtps_participant_ != nullptr) {
      participant_lock_fd_ = scoped_lock_fd.Release();
      break;
    }
    AWARN << "participant id " << participant_id
          << " is unavailable, trying the next slot";
  }
  if (fastrtps_participant_ == nullptr) {
    AERROR << "failed to allocate an RTPS participant after "
           << kParticipantIdRange << " attempts";
    return;
  }
  eprosima::fastrtps::Domain::registerType(fastrtps_participant_, &type_);
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

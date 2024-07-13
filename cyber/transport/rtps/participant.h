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

#ifndef CYBER_TRANSPORT_RTPS_PARTICIPANT_H_
#define CYBER_TRANSPORT_RTPS_PARTICIPANT_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "fastdds/rtps/common/Locator.hpp"

#include "cyber/transport/rtps/underlay_message_type.h"

namespace apollo {
namespace cyber {
namespace transport {

class Participant;
using ParticipantPtr = std::shared_ptr<Participant>;

class Participant {
 public:
  Participant(const std::string& name, int send_port,
              eprosima::fastdds::rtps::RTPSParticipantListener* listener = nullptr);
  virtual ~Participant();

  void Shutdown();

  eprosima::fastdds::rtps::RTPSParticipant* fastrtps_participant();
  bool is_shutdown() const { return shutdown_.load(); }

 private:
  void CreateFastRtpsParticipant(
      const std::string& name, int send_port,
      eprosima::fastdds::rtps::RTPSParticipantListener* listener);

  std::atomic<bool> shutdown_;
  std::string name_;
  int send_port_;
  eprosima::fastdds::rtps::RTPSParticipantListener* listener_;
  UnderlayMessageType type_;
  eprosima::fastdds::rtps::RTPSParticipant* fastrtps_participant_;
  std::mutex mutex_;
};

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_RTPS_PARTICIPANT_H_

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

#ifndef CYBER_TRANSPORT_RTPS_SUB_LISTENER_H_
#define CYBER_TRANSPORT_RTPS_SUB_LISTENER_H_

#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include "fastdds/dds/subscriber/SampleInfo.hpp"
#include "fastdds/dds/subscriber/DataReader.hpp"
#include "fastdds/dds/subscriber/DataReaderListener.hpp"
#include "fastdds/dds/topic/TopicDescription.hpp"
// #include "fastdds/dds/subscriber/Subscriber.hpp"
// #include "fastdds/dds/subscriber/SubscriberListener.hpp"
#include "fastdds/rtps/common/MatchingInfo.hpp"

#include "cyber/transport/message/message_info.h"
#include "cyber/transport/rtps/underlay_message.h"
#include "cyber/transport/rtps/underlay_message_type.h"

namespace apollo {
namespace cyber {
namespace transport {

class SubListener;
using SubListenerPtr = std::shared_ptr<SubListener>;

class SubListener : public eprosima::fastdds::dds::DataReaderListener {
 public:
  using NewMsgCallback = std::function<void(
      uint64_t channel_id, const std::shared_ptr<std::string>& msg_str,
      const MessageInfo& msg_info)>;

  explicit SubListener(const NewMsgCallback& callback);
  virtual ~SubListener();

  virtual void on_data_available(eprosima::fastdds::dds::DataReader* reader) override;

 private:
  NewMsgCallback callback_;
  MessageInfo msg_info_;
  std::mutex mutex_;
};

}  // namespace transport
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TRANSPORT_RTPS_SUB_LISTENER_H_

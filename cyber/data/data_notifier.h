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

#ifndef CYBER_DATA_DATA_NOTIFIER_H_
#define CYBER_DATA_DATA_NOTIFIER_H_

#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "cyber/common/log.h"
#include "cyber/common/macros.h"
#include "cyber/data/cache_buffer.h"
#include "cyber/event/perf_event_cache.h"
#include "cyber/time/time.h"

namespace apollo {
namespace cyber {
namespace data {

using apollo::cyber::Time;
using apollo::cyber::event::PerfEventCache;

struct Notifier {
  std::function<void()> callback;
};

class DataNotifier {
 public:
  using NotifyVector = std::vector<std::shared_ptr<Notifier>>;
  using NotifySnapshot = std::shared_ptr<const NotifyVector>;
  ~DataNotifier() {}

  void AddNotifier(uint64_t channel_id,
                   const std::shared_ptr<Notifier>& notifier);
  void RemoveNotifier(uint64_t channel_id,
                      const std::shared_ptr<Notifier>& notifier);

  bool Notify(const uint64_t channel_id);

 private:
  std::shared_mutex notifies_map_mutex_;
  std::unordered_map<uint64_t, NotifySnapshot> notifies_map_;

  DECLARE_SINGLETON(DataNotifier)
};

inline DataNotifier::DataNotifier() {}

inline void DataNotifier::AddNotifier(
    uint64_t channel_id, const std::shared_ptr<Notifier>& notifier) {
  std::unique_lock<std::shared_mutex> lock(notifies_map_mutex_);
  auto it = notifies_map_.find(channel_id);
  if (it != notifies_map_.end()) {
    auto notifies = std::make_shared<NotifyVector>(*it->second);
    notifies->emplace_back(notifier);
    it->second = std::move(notifies);
  } else {
    notifies_map_.emplace(
        channel_id, std::make_shared<const NotifyVector>(NotifyVector{notifier}));
  }
}

inline void DataNotifier::RemoveNotifier(
    uint64_t channel_id, const std::shared_ptr<Notifier>& notifier) {
  std::unique_lock<std::shared_mutex> lock(notifies_map_mutex_);
  auto it = notifies_map_.find(channel_id);
  if (it == notifies_map_.end()) {
    return;
  }
  auto notifies = std::make_shared<NotifyVector>(*it->second);
  notifies->erase(
      std::remove(notifies->begin(), notifies->end(), notifier),
      notifies->end());
  it->second = std::move(notifies);
}

inline bool DataNotifier::Notify(const uint64_t channel_id) {
  NotifySnapshot notifiers;
  {
    std::shared_lock<std::shared_mutex> lock(notifies_map_mutex_);
    auto it = notifies_map_.find(channel_id);
    if (it == notifies_map_.end()) {
      return false;
    }
    notifiers = it->second;
  }
  for (const auto& notifier : *notifiers) {
    if (notifier && notifier->callback) {
      notifier->callback();
    }
  }
  return true;
}

}  // namespace data
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_DATA_DATA_NOTIFIER_H_

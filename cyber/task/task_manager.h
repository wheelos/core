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

#ifndef CYBER_TASK_TASK_MANAGER_H_
#define CYBER_TASK_TASK_MANAGER_H_

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/base/bounded_queue.h"
#include "cyber/common/log.h"
#include "cyber/scheduler/scheduler_factory.h"

namespace apollo {
namespace cyber {

namespace task_internal {

class TaskStateBase {
 public:
  virtual ~TaskStateBase() = default;
  virtual void Cancel() = 0;
};

template <typename ReturnType>
class TaskState final : public TaskStateBase {
 public:
  explicit TaskState(std::packaged_task<ReturnType()>&& task)
      : task_(std::make_shared<std::packaged_task<ReturnType()>>(
            std::move(task))) {}

  std::future<ReturnType> GetFuture() { return task_->get_future(); }

  void Run() {
    std::shared_ptr<std::packaged_task<ReturnType()>> task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task = std::move(task_);
    }
    if (task) {
      (*task)();
    }
  }

  void Cancel() override {
    std::shared_ptr<std::packaged_task<ReturnType()>> task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task = std::move(task_);
    }
    task.reset();
  }

 private:
  std::mutex mutex_;
  std::shared_ptr<std::packaged_task<ReturnType()>> task_;
};

enum class EnqueueResult {
  kAccepted,
  kStopped,
  kFull,
};

class TaskManagerLifecycle {
 public:
  template <typename Task>
  EnqueueResult Enqueue(
      const std::shared_ptr<Task>& task,
      base::BoundedQueue<std::function<void()>>* task_queue,
      std::function<void()>&& work) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_.load()) {
      return EnqueueResult::kStopped;
    }
    pending_tasks_[task.get()] = task;
    if (!task_queue->Enqueue(std::move(work))) {
      pending_tasks_.erase(task.get());
      return EnqueueResult::kFull;
    }
    return EnqueueResult::kAccepted;
  }

  void Complete(const TaskStateBase* task) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_tasks_.erase(task);
  }

  bool exchange(bool stopped) {
    std::vector<std::shared_ptr<TaskStateBase>> pending_tasks;
    bool was_stopped = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      was_stopped = stopped_.load();
      if (was_stopped == stopped) {
        return was_stopped;
      }
      stopped_.store(stopped);
      if (stopped) {
        pending_tasks.reserve(pending_tasks_.size());
        for (auto& entry : pending_tasks_) {
          pending_tasks.push_back(std::move(entry.second));
        }
        pending_tasks_.clear();
      }
    }
    for (auto& task : pending_tasks) {
      task->Cancel();
    }
    return was_stopped;
  }

  bool load() const { return stopped_.load(); }
  explicit operator bool() const { return load(); }

 private:
  std::atomic<bool> stopped_{false};
  std::mutex mutex_;
  std::unordered_map<const TaskStateBase*, std::shared_ptr<TaskStateBase>>
      pending_tasks_;
};

}  // namespace task_internal

class TaskManager {
 public:
  virtual ~TaskManager();

  void Shutdown();

  template <typename F, typename... Args>
  auto Enqueue(F&& func, Args&&... args)
      -> std::future<typename std::result_of<F(Args...)>::type> {
    using return_type = typename std::result_of<F(Args...)>::type;
    auto task = std::make_shared<task_internal::TaskState<return_type>>(
        std::packaged_task<return_type()>(
            std::bind(std::forward<F>(func), std::forward<Args>(args)...)));
    std::future<return_type> result = task->GetFuture();
    const auto enqueue_result = stop_.Enqueue(
        task, task_queue_.get(), [this, task]() {
          task->Run();
          stop_.Complete(task.get());
        });
    if (enqueue_result == task_internal::EnqueueResult::kStopped) {
      AERROR << "Task enqueue rejected: task manager is stopped.";
      return std::future<return_type>();
    }
    if (enqueue_result == task_internal::EnqueueResult::kFull) {
      AERROR << "Task enqueue rejected: task queue is full.";
      return std::future<return_type>();
    }
    for (auto& task_id : tasks_) {
      scheduler::Instance()->NotifyTask(task_id);
    }
    return result;
  }

 private:
  uint32_t num_threads_ = 0;
  uint32_t task_queue_size_ = 1000;
  task_internal::TaskManagerLifecycle stop_;
  std::vector<uint64_t> tasks_;
  std::shared_ptr<base::BoundedQueue<std::function<void()>>> task_queue_;
  DECLARE_SINGLETON(TaskManager);
};

}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_TASK_TASK_MANAGER_H_

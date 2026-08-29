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

#include "cyber/base/signal.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "gtest/gtest.h"

namespace apollo {
namespace cyber {
namespace base {

TEST(SlotTest, zero_input_param) {
  char ch = '0';
  Slot<> slot_a([&ch]() { ch = 'a'; });
  EXPECT_TRUE(slot_a.connected());

  slot_a();
  EXPECT_EQ(ch, 'a');

  slot_a.Disconnect();
  EXPECT_FALSE(slot_a.connected());

  ch = '0';
  slot_a();
  EXPECT_NE(ch, 'a');

  Slot<> slot_b([&ch]() { ch = 'b'; }, false);
  EXPECT_FALSE(slot_b.connected());

  ch = '0';
  slot_b();
  EXPECT_NE(ch, 'b');

  Slot<> slot_c(nullptr);
  EXPECT_NO_FATAL_FAILURE(slot_c());
}

TEST(SlotTest, two_input_params) {
  int sum = 0;
  Slot<int, int> slot_a([&sum](int lhs, int rhs) { sum = lhs + rhs; });
  EXPECT_TRUE(slot_a.connected());

  int lhs = 1, rhs = 2;
  slot_a(lhs, rhs);
  EXPECT_EQ(sum, lhs + rhs);

  Slot<int, int> slot_b(slot_a);
  lhs = 3;
  rhs = 4;
  slot_b(lhs, rhs);
  EXPECT_EQ(sum, lhs + rhs);

  slot_b.Disconnect();
  EXPECT_FALSE(slot_b.connected());

  sum = 0;
  lhs = 5;
  rhs = 6;
  slot_b(lhs, rhs);
  EXPECT_EQ(sum, 0);
}

TEST(ConnectionTest, null_signal) {
  Connection<> conn_a;
  EXPECT_FALSE(conn_a.IsConnected());
  EXPECT_FALSE(conn_a.Disconnect());
  EXPECT_FALSE(conn_a.HasSlot(nullptr));

  auto slot = std::make_shared<Slot<>>([]() {});
  Connection<> conn_b(slot, nullptr);
  EXPECT_TRUE(conn_b.IsConnected());
  EXPECT_FALSE(conn_b.Disconnect());
  EXPECT_TRUE(conn_b.HasSlot(slot));

  EXPECT_FALSE(conn_a.HasSlot(slot));

  conn_b = conn_b;
  conn_a = conn_b;
  EXPECT_TRUE(conn_a.IsConnected());
  EXPECT_FALSE(conn_a.Disconnect());
  EXPECT_TRUE(conn_a.HasSlot(slot));

  Signal<> sig;
  Connection<> conn_c(nullptr, &sig);
  EXPECT_FALSE(conn_c.Disconnect());
}

TEST(SignalTest, module) {
  Signal<int, int> sig;

  int sum_a = 0;
  auto conn_a = sig.Connect([&sum_a](int lhs, int rhs) { sum_a = lhs + rhs; });

  int sum_b = 0;
  auto conn_b = sig.Connect([&sum_b](int lhs, int rhs) { sum_b = lhs + rhs; });

  int lhs = 1, rhs = 2;
  sig(lhs, rhs);
  EXPECT_EQ(sum_a, lhs + rhs);
  EXPECT_EQ(sum_b, lhs + rhs);

  Connection<int, int> conn_c;
  EXPECT_FALSE(sig.Disconnect(conn_c));
  EXPECT_TRUE(sig.Disconnect(conn_b));
  sum_a = 0;
  sum_b = 0;
  lhs = 3;
  rhs = 4;
  sig(lhs, rhs);
  EXPECT_EQ(sum_a, lhs + rhs);
  EXPECT_NE(sum_b, lhs + rhs);

  sig.DisconnectAllSlots();
  sum_a = 0;
  sum_b = 0;
  lhs = 5;
  rhs = 6;
  sig(lhs, rhs);
  EXPECT_NE(sum_a, lhs + rhs);
  EXPECT_NE(sum_b, lhs + rhs);
}

TEST(SignalTest, disconnect_waits_for_active_callback) {
  Signal<> signal;
  std::promise<void> callback_started;
  std::promise<void> release_callback;
  auto release_future = release_callback.get_future().share();
  auto connection = signal.Connect([&]() {
    callback_started.set_value();
    release_future.wait();
  });

  std::thread callback_thread([&]() { signal(); });
  callback_started.get_future().wait();

  std::promise<bool> disconnect_finished;
  auto disconnect_finished_future = disconnect_finished.get_future();
  std::thread disconnect_thread([&]() {
    disconnect_finished.set_value(connection.Disconnect());
  });
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (connection.IsConnected() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(connection.IsConnected());
  EXPECT_EQ(disconnect_finished_future.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  release_callback.set_value();
  callback_thread.join();
  disconnect_thread.join();
  EXPECT_TRUE(disconnect_finished_future.get());
}

TEST(SignalTest, disconnect_waits_after_disconnect_all_detaches_slot) {
  Signal<> signal;
  std::promise<void> callback_started;
  std::promise<void> release_callback;
  auto release_future = release_callback.get_future().share();
  auto connection = signal.Connect([&]() {
    callback_started.set_value();
    release_future.wait();
  });

  std::thread callback_thread([&]() { signal(); });
  callback_started.get_future().wait();
  std::thread disconnect_all_thread([&]() { signal.DisconnectAllSlots(); });

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (connection.IsConnected() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(connection.IsConnected());

  auto direct_disconnect =
      std::async(std::launch::async, [&]() { return connection.Disconnect(); });
  EXPECT_EQ(direct_disconnect.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  release_callback.set_value();
  callback_thread.join();
  disconnect_all_thread.join();
  EXPECT_TRUE(direct_disconnect.get());
}

TEST(SignalTest, callback_can_disconnect_itself) {
  Signal<> signal;
  Connection<> connection;
  connection = signal.Connect([&]() { EXPECT_TRUE(connection.Disconnect()); });

  EXPECT_NO_FATAL_FAILURE(signal());
  EXPECT_FALSE(connection.IsConnected());
}

TEST(SignalTest, nested_callback_can_disconnect_outer_slot) {
  auto outer_signal = std::make_shared<Signal<>>();
  auto inner_signal = std::make_shared<Signal<>>();
  auto outer_connection = std::make_shared<Connection<>>();
  std::weak_ptr<Connection<>> weak_outer_connection = outer_connection;
  *outer_connection =
      outer_signal->Connect([inner_signal]() { (*inner_signal)(); });
  inner_signal->Connect([weak_outer_connection]() {
    auto connection = weak_outer_connection.lock();
    if (connection) {
      connection->Disconnect();
    }
  });

  auto completed = std::make_shared<std::promise<bool>>();
  auto completed_future = completed->get_future();
  std::thread([outer_signal, outer_connection, completed]() {
    (*outer_signal)();
    completed->set_value(!outer_connection->IsConnected());
  }).detach();

  EXPECT_EQ(completed_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  if (completed_future.wait_for(std::chrono::seconds(0)) ==
      std::future_status::ready) {
    EXPECT_TRUE(completed_future.get());
  }
}

TEST(SignalTest, callback_disconnect_waits_for_other_same_slot_callback) {
  Signal<int> signal;
  Connection<int> connection;
  std::promise<void> first_started;
  auto first_started_future = first_started.get_future();
  std::promise<void> release_first;
  auto release_first_future = release_first.get_future().share();
  std::promise<bool> disconnect_finished;
  auto disconnect_finished_future = disconnect_finished.get_future();

  connection = signal.Connect([&](int invocation) {
    if (invocation == 0) {
      first_started.set_value();
      release_first_future.wait();
      return;
    }
    disconnect_finished.set_value(connection.Disconnect());
  });

  std::thread first_thread([&]() { signal(0); });
  first_started_future.wait();
  std::thread disconnect_thread([&]() { signal(1); });
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (connection.IsConnected() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(connection.IsConnected());

  EXPECT_EQ(disconnect_finished_future.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  release_first.set_value();
  first_thread.join();
  disconnect_thread.join();
  EXPECT_TRUE(disconnect_finished_future.get());
  EXPECT_FALSE(connection.IsConnected());
}

TEST(SignalTest, callbacks_can_disconnect_each_other_without_deadlock) {
  Signal<> first_signal;
  Signal<> second_signal;
  Connection<> first_connection;
  Connection<> second_connection;
  std::promise<void> first_started;
  std::promise<void> second_started;
  auto first_started_future = first_started.get_future().share();
  auto second_started_future = second_started.get_future().share();

  first_connection = first_signal.Connect([&]() {
    first_started.set_value();
    second_started_future.wait();
    EXPECT_TRUE(second_connection.Disconnect());
  });
  second_connection = second_signal.Connect([&]() {
    second_started.set_value();
    first_started_future.wait();
    EXPECT_TRUE(first_connection.Disconnect());
  });

  auto first = std::async(std::launch::async, [&]() { first_signal(); });
  auto second = std::async(std::launch::async, [&]() { second_signal(); });

  EXPECT_EQ(first.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_EQ(second.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
}

TEST(SignalTest, concurrent_self_disconnects_do_not_deadlock) {
  Signal<> signal;
  Connection<> connection;
  std::atomic<int> entered(0);
  std::promise<void> both_entered;
  auto both_entered_future = both_entered.get_future().share();

  connection = signal.Connect([&]() {
    if (entered.fetch_add(1) + 1 == 2) {
      both_entered.set_value();
    }
    both_entered_future.wait();
    EXPECT_TRUE(connection.Disconnect());
  });

  auto first = std::async(std::launch::async, [&]() { signal(); });
  auto second = std::async(std::launch::async, [&]() { signal(); });

  EXPECT_EQ(first.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_EQ(second.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
}

TEST(SignalTest, external_disconnect_waits_for_self_disconnect_callback) {
  Signal<int> signal;
  Connection<int> connection;
  std::promise<void> blocker_started;
  auto blocker_started_future = blocker_started.get_future().share();
  std::promise<void> self_disconnect_started;
  auto self_disconnect_started_future =
      self_disconnect_started.get_future().share();
  std::promise<void> release_blocker;
  auto release_blocker_future = release_blocker.get_future().share();
  std::promise<void> release_self_disconnect;
  auto release_self_disconnect_future =
      release_self_disconnect.get_future().share();

  connection = signal.Connect([&](int invocation) {
    if (invocation == 0) {
      blocker_started.set_value();
      release_blocker_future.wait();
      return;
    }
    self_disconnect_started.set_value();
    connection.Disconnect();
    release_self_disconnect_future.wait();
  });

  auto blocker = std::async(std::launch::async, [&]() { signal(0); });
  blocker_started_future.wait();
  auto self_disconnect =
      std::async(std::launch::async, [&]() { signal(1); });
  self_disconnect_started_future.wait();

  auto external_disconnect =
      std::async(std::launch::async, [&]() { return connection.Disconnect(); });
  release_blocker.set_value();
  EXPECT_EQ(external_disconnect.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  release_self_disconnect.set_value();
  EXPECT_EQ(external_disconnect.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_TRUE(external_disconnect.get());
  blocker.get();
  self_disconnect.get();
}

}  // namespace base
}  // namespace cyber
}  // namespace apollo

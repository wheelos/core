/******************************************************************************
 * Copyright 2026 The Apollo Authors. All Rights Reserved.
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

#include "cyber/service/client.h"

#include "gtest/gtest.h"

namespace apollo {
namespace cyber {

class SuccessfulTransmitter : public transport::Transmitter<int> {
 public:
  SuccessfulTransmitter() : transport::Transmitter<int>(proto::RoleAttributes()) {}

  void Enable() override {}
  void Disable() override {}
  bool Transmit(const MessagePtr& message,
                const transport::MessageInfo& message_info) override {
    (void)message;
    (void)message_info;
    return true;
  }
};

class IdleReceiver : public transport::Receiver<int> {
 public:
  IdleReceiver()
      : transport::Receiver<int>(
            proto::RoleAttributes(),
            [](const MessagePtr&, const transport::MessageInfo&,
               const proto::RoleAttributes&) {}) {}

  void Enable() override {}
  void Disable() override {}
  void Enable(const proto::RoleAttributes&) override {}
  void Disable(const proto::RoleAttributes&) override {}
};

class ClientTestPeer {
 public:
  template <typename Request, typename Response>
  static void SetEndpoints(
      Client<Request, Response>* client,
      const std::shared_ptr<transport::Transmitter<Request>>& transmitter,
      const std::shared_ptr<transport::Receiver<Response>>& receiver) {
    client->request_transmitter_ = transmitter;
    client->response_receiver_ = receiver;
    client->writer_id_ = transmitter->id();
  }

  template <typename Request, typename Response>
  static typename Client<Request, Response>::SharedPromise AddPendingRequest(
      Client<Request, Response>* client, uint64_t sequence_number) {
    using TestClient = Client<Request, Response>;
    auto promise = std::make_shared<typename TestClient::Promise>();
    typename TestClient::SharedFuture future(promise->get_future());
    client->pending_requests_[sequence_number] = std::make_tuple(
        promise, [](typename TestClient::SharedFuture) {}, future);
    return promise;
  }

  template <typename Request, typename Response>
  static bool ErasePendingRequest(
      Client<Request, Response>* client, uint64_t sequence_number,
      const typename Client<Request, Response>::SharedPromise& promise) {
    return client->ErasePendingRequest(sequence_number, promise);
  }

  template <typename Request, typename Response>
  static bool HasPendingRequest(Client<Request, Response>* client,
                                uint64_t sequence_number) {
    return client->pending_requests_.count(sequence_number) != 0;
  }

  template <typename Request, typename Response>
  static size_t PendingRequestCount(Client<Request, Response>* client) {
    std::lock_guard<std::mutex> lock(client->pending_requests_mutex_);
    return client->pending_requests_.size();
  }
};

TEST(ClientTest, send_request_timeout_erases_pending_request) {
  Client<int, int> client("client_test", "service_test");
  auto transmitter = std::make_shared<SuccessfulTransmitter>();
  auto receiver = std::make_shared<IdleReceiver>();
  ClientTestPeer::SetEndpoints<int, int>(&client, transmitter, receiver);

  EXPECT_EQ(nullptr,
            client.SendRequest(std::make_shared<int>(1),
                               std::chrono::seconds(0)));
  EXPECT_EQ(0, ClientTestPeer::PendingRequestCount(&client));
}

TEST(ClientTest, timeout_does_not_erase_reused_sequence) {
  Client<int, int> client("client_test", "service_test");
  auto timed_out_promise = ClientTestPeer::AddPendingRequest(&client, 1);
  auto reused_sequence_promise = ClientTestPeer::AddPendingRequest(&client, 1);

  EXPECT_FALSE(
      ClientTestPeer::ErasePendingRequest(&client, 1, timed_out_promise));
  EXPECT_TRUE(ClientTestPeer::HasPendingRequest(&client, 1));
  EXPECT_TRUE(
      ClientTestPeer::ErasePendingRequest(&client, 1, reused_sequence_promise));
}

}  // namespace cyber
}  // namespace apollo

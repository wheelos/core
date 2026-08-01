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

#include "cyber/node/node.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

#include "gtest/gtest.h"

#include "cyber/proto/unit_test.pb.h"

#include "cyber/cyber.h"
#include "cyber/init.h"
#include "cyber/node/reader.h"
#include "cyber/node/writer.h"

namespace apollo {
namespace cyber {

using apollo::cyber::proto::Chatter;
using apollo::cyber::proto::ChatterBenchmark;

TEST(NodeTest, cases) {
  auto node = CreateNode("node_test");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Name(), "node_test");

  proto::RoleAttributes attr;
  attr.set_channel_name("/node_test_channel");
  auto channel_id = common::GlobalData::RegisterChannel(attr.channel_name());
  attr.set_channel_id(channel_id);
  attr.mutable_qos_profile()->set_depth(10);

  auto reader = node->CreateReader<Chatter>(attr);
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(node->GetReader<Chatter>(attr.channel_name()));

  auto writer = node->CreateWriter<Chatter>(attr);
  ASSERT_NE(writer, nullptr);
  auto server = node->CreateService<Chatter, Chatter>(
      "node_test_server", [](const std::shared_ptr<Chatter>& request,
                             std::shared_ptr<Chatter>& response) {
        AINFO << "server: I am server";
        static uint64_t id = 0;
        ++id;
        response->set_seq(id);
        response->set_timestamp(0);
      });
  ASSERT_NE(server, nullptr);
  auto client = node->CreateClient<Chatter, Chatter>("node_test_server");
  ASSERT_NE(client, nullptr);
  auto chatter_msg = std::make_shared<Chatter>();
  chatter_msg->set_seq(0);
  chatter_msg->set_timestamp(0);
  auto res = client->SendRequest(chatter_msg);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->seq(), 1);

  node->Observe();
  node->ClearData();
}

TEST(NodeTest, ServiceReadinessAndLifecycle) {
  auto node = CreateNode("node_service_lifecycle_test");
  ASSERT_NE(node, nullptr);

  auto client =
      node->CreateClient<Chatter, Chatter>("node_service_lifecycle_server");
  ASSERT_NE(client, nullptr);
  EXPECT_FALSE(client->ServiceIsReady());
  EXPECT_FALSE(client->WaitForService(std::chrono::milliseconds(20)));

  auto request = std::make_shared<Chatter>();
  request->set_seq(0);
  EXPECT_EQ(client->SendRequest(request, std::chrono::seconds(0)), nullptr);

  std::atomic<uint64_t> handled_requests{0};
  auto server = node->CreateService<Chatter, Chatter>(
      "node_service_lifecycle_server",
      [&handled_requests](const std::shared_ptr<Chatter>&,
                          std::shared_ptr<Chatter>& response) {
        response->set_seq(++handled_requests);
      });
  ASSERT_NE(server, nullptr);
  EXPECT_TRUE(client->ServiceIsReady());
  EXPECT_TRUE(client->WaitForService(std::chrono::milliseconds(20)));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(handled_requests.load(), 0);
  auto response = client->SendRequest(request);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->seq(), 1);

  server.reset();
  EXPECT_FALSE(client->ServiceIsReady());
}

TEST(NodeTest, ServiceCallbackCanSendAnotherRequest) {
  auto node = CreateNode("node_service_callback_test");
  ASSERT_NE(node, nullptr);

  auto server = node->CreateService<Chatter, Chatter>(
      "node_service_callback_server",
      [](const std::shared_ptr<Chatter>& request,
         std::shared_ptr<Chatter>& response) {
        response->set_seq(request->seq() + 1);
      });
  ASSERT_NE(server, nullptr);
  auto client =
      node->CreateClient<Chatter, Chatter>("node_service_callback_server");
  ASSERT_NE(client, nullptr);

  auto completed = std::make_shared<std::promise<uint64_t>>();
  auto completed_future = completed->get_future();
  auto first_request = std::make_shared<Chatter>();
  first_request->set_seq(0);
  auto first_future = client->AsyncSendRequest(
      first_request,
      [client, completed](Client<Chatter, Chatter>::SharedFuture response) {
        auto first_response = response.get();
        auto second_request = std::make_shared<Chatter>();
        second_request->set_seq(first_response->seq());
        client->AsyncSendRequest(
            second_request,
            [completed](Client<Chatter, Chatter>::SharedFuture second_response) {
              completed->set_value(second_response.get()->seq());
            });
      });
  ASSERT_TRUE(first_future.valid());
  ASSERT_EQ(completed_future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  EXPECT_EQ(completed_future.get(), 2);
}

TEST(NodeTest, ServiceV2CallOptionsAndConcurrency) {
  auto node = CreateNode("node_service_v2_test");
  ASSERT_NE(node, nullptr);

  ServiceOptions service_options;
  service_options.concurrency = 2;
  service_options.max_pending_requests = 8;
  std::atomic<uint64_t> handler_calls{0};
  auto server = node->CreateService<Chatter, Chatter>(
      "node_service_v2_server", service_options,
      [&handler_calls](RpcContext& context,
                       const std::shared_ptr<Chatter>& request,
                       std::shared_ptr<Chatter>& response) -> RpcStatus {
        ++handler_calls;
        if (request->seq() == 55) {
          while (!context.IsCancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          return RpcStatus::OK();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (context.IsCancelled()) {
          return RpcStatus(RpcStatusCode::DEADLINE_EXCEEDED,
                           "handler deadline exceeded");
        }
        if (request->seq() == 99) {
          return RpcStatus(RpcStatusCode::FAILED_PRECONDITION,
                           "request rejected");
        }
        response->set_seq(request->seq() + 1);
        return RpcStatus::OK();
      });
  ASSERT_NE(server, nullptr);
  auto duplicate = node->CreateService<Chatter, Chatter>(
      "node_service_v2_server", service_options,
      [](const std::shared_ptr<Chatter>&, std::shared_ptr<Chatter>&) {});
  EXPECT_EQ(duplicate, nullptr);

  auto incompatible_client =
      node->CreateClient<Chatter, ChatterBenchmark>("node_service_v2_server");
  ASSERT_NE(incompatible_client, nullptr);
  EXPECT_FALSE(incompatible_client->ServiceIsReady());

  auto client =
      node->CreateClient<Chatter, Chatter>("node_service_v2_server");
  ASSERT_NE(client, nullptr);

  Chatter request;
  request.set_seq(7);
  RpcCallOptions short_call;
  short_call.timeout = std::chrono::milliseconds(1);
  auto timed_out = client->Call(request, short_call);
  EXPECT_FALSE(timed_out.ok());
  EXPECT_EQ(timed_out.status().code(), RpcStatusCode::DEADLINE_EXCEEDED);

  RpcCallOptions normal_call;
  normal_call.timeout = std::chrono::seconds(1);
  auto completed = client->Call(request, normal_call);
  ASSERT_TRUE(completed.ok());
  ASSERT_NE(completed.response(), nullptr);
  EXPECT_EQ(completed.response()->seq(), 8);

  request.set_seq(99);
  auto rejected = client->Call(request, normal_call);
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.status().code(), RpcStatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(rejected.status().message(), "request rejected");

  request.set_seq(10);
  RpcCallOptions idempotent_call = normal_call;
  idempotent_call.idempotency_key = "service-v2-dedup";
  auto first_idempotent = client->Call(request, idempotent_call);
  ASSERT_TRUE(first_idempotent.ok());
  EXPECT_EQ(first_idempotent.response()->seq(), 11);
  const auto calls_after_first = handler_calls.load();

  request.set_seq(20);
  auto repeated_idempotent = client->Call(request, idempotent_call);
  ASSERT_TRUE(repeated_idempotent.ok());
  EXPECT_EQ(repeated_idempotent.response()->seq(), 11);
  EXPECT_EQ(handler_calls.load(), calls_after_first);

  request.set_seq(55);
  uint64_t call_id = 0;
  auto cancellable = client->AsyncCall(request, normal_call, &call_id);
  ASSERT_TRUE(cancellable.valid());
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(client->CancelRequest(call_id));
  ASSERT_EQ(cancellable.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_EQ(cancellable.get().status().code(), RpcStatusCode::CANCELLED);

  auto invalid = client->Call(
      std::shared_ptr<Chatter>(), normal_call);
  EXPECT_EQ(invalid.status().code(), RpcStatusCode::INVALID_ARGUMENT);
}

TEST(NodeTest, ReplicatedServiceRoutingAndFailover) {
  auto server_node = CreateNode("replicated_service_node");
  ASSERT_NE(server_node, nullptr);

  std::atomic<uint64_t> first_calls{0};
  std::atomic<uint64_t> second_calls{0};
  ServiceOptions first_options;
  first_options.instance_policy = ServiceInstancePolicy::REPLICATED;
  first_options.instance_id = 1;
  auto first_server = server_node->CreateService<Chatter, Chatter>(
      "replicated_service", first_options,
      [&first_calls](const std::shared_ptr<Chatter>& request,
                     std::shared_ptr<Chatter>& response) {
        ++first_calls;
        response->set_seq(request->seq() + 100);
      });
  ASSERT_NE(first_server, nullptr);

  ServiceOptions second_options = first_options;
  second_options.instance_id = 2;
  auto second_server = server_node->CreateService<Chatter, Chatter>(
      "replicated_service", second_options,
      [&second_calls](const std::shared_ptr<Chatter>& request,
                      std::shared_ptr<Chatter>& response) {
        ++second_calls;
        response->set_seq(request->seq() + 200);
      });
  ASSERT_NE(second_server, nullptr);

  std::vector<std::shared_ptr<Client<Chatter, Chatter>>> clients;
  for (uint64_t i = 0; i < 8; ++i) {
    auto client_node =
        CreateNode("replicated_client_" + std::to_string(i));
    ASSERT_NE(client_node, nullptr);
    auto client =
        client_node->CreateClient<Chatter, Chatter>("replicated_service");
    ASSERT_NE(client, nullptr);
    Chatter request;
    request.set_seq(i);
    auto result = client->Call(request);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.response()->seq() == i + 100 ||
                result.response()->seq() == i + 200);
    clients.emplace_back(std::move(client));
  }
  EXPECT_EQ(first_calls.load() + second_calls.load(), clients.size());
  EXPECT_GT(first_calls.load(), 0);
  EXPECT_GT(second_calls.load(), 0);

  first_server.reset();
  const auto first_calls_after_stop = first_calls.load();
  for (uint64_t i = 0; i < clients.size(); ++i) {
    Chatter request;
    request.set_seq(i);
    auto result = clients[i]->Call(request);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.response()->seq(), i + 200);
  }
  EXPECT_EQ(first_calls.load(), first_calls_after_stop);
}

}  // namespace cyber
}  // namespace apollo

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  apollo::cyber::Init(argv[0]);
  const int ret = RUN_ALL_TESTS();
  std::fflush(nullptr);
  _Exit(ret);
}

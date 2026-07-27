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

#include "cyber/parameter/parameter_client.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "cyber/cyber.h"
#include "cyber/init.h"
#include "cyber/message/protobuf_factory.h"
#include "cyber/parameter/parameter_server.h"

namespace apollo {
namespace cyber {

class ParameterClientTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    apollo::cyber::Init("parameter_client_test");
  }

  virtual void SetUp() {
    static size_t node_id = 0;
    const std::string server_node_name =
        "parameter_server_" + std::to_string(node_id++);
    server_node_ = std::shared_ptr<Node>(CreateNode(server_node_name).release());
    client_node_ = std::shared_ptr<Node>(
        CreateNode(server_node_name + "_client").release());
    ASSERT_NE(server_node_, nullptr);
    ASSERT_NE(client_node_, nullptr);
    ps_.reset(new ParameterServer(server_node_));
    pc_.reset(new ParameterClient(client_node_, server_node_name));
  }

  virtual void TearDown() {
    ps_.reset();
    pc_.reset();
    client_node_.reset();
    server_node_.reset();
  }

  template <typename Predicate>
  bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
  }

 protected:
  std::shared_ptr<Node> server_node_;
  std::shared_ptr<Node> client_node_;
  std::unique_ptr<ParameterServer> ps_;
  std::unique_ptr<ParameterClient> pc_;
};

TEST_F(ParameterClientTest, set_parameter) {
  ASSERT_TRUE(WaitFor([&]() { return pc_->SetParameter(Parameter("int", 1)); },
                      std::chrono::seconds(3)));
  Parameter parameter;
  ASSERT_TRUE(ps_->GetParameter("int", &parameter));
  EXPECT_EQ("int", parameter.Name());
  EXPECT_EQ(1, parameter.AsInt64());
}

TEST_F(ParameterClientTest, get_parameter) {
  ps_->SetParameter(Parameter("int", 1));
  Parameter parameter;
  ASSERT_TRUE(WaitFor([&]() { return pc_->GetParameter("int", &parameter); },
                      std::chrono::seconds(3)));
  EXPECT_EQ("int", parameter.Name());
  EXPECT_EQ(1, parameter.AsInt64());
  EXPECT_FALSE(pc_->GetParameter("double", &parameter));
}

TEST_F(ParameterClientTest, list_parameter) {
  ps_->SetParameter(Parameter("int", 1));
  std::vector<Parameter> parameters;
  ASSERT_TRUE(WaitFor(
      [&]() {
        parameters.clear();
        return pc_->ListParameters(&parameters) && parameters.size() == 1;
      },
      std::chrono::seconds(3)));
  ASSERT_EQ(1, parameters.size());
  EXPECT_EQ("int", parameters[0].Name());
  EXPECT_EQ(1, parameters[0].AsInt64());
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

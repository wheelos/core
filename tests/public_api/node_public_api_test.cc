#include "gtest/gtest.h"

#include "cyber/proto/parameter.pb.h"

#include "cyber/init.h"
#include "cyber/node/node.h"

TEST(NodePublicApiTest, CreatesNodeAndWriterWithNarrowDependencies) {
  ASSERT_TRUE(apollo::cyber::Init("node_public_api_test"));
  {
    auto node = apollo::cyber::CreateNode("public_api_node");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->Name(), "public_api_node");
    auto writer =
        node->CreateWriter<apollo::cyber::proto::Param>("/public_api/output");
    EXPECT_NE(writer, nullptr);
  }
  apollo::cyber::Clear();
}

#include "cyber/blocker/blocker_manager.h"

#include "gtest/gtest.h"

#include "cyber/proto/unit_test.pb.h"

namespace apollo {
namespace cyber {
namespace blocker {

TEST(BlockerTest, blocker_manager_test) {
  auto block_mgr = BlockerManager::Instance();
  Blocker<proto::UnitTest>::MessageType msgtype;
  auto callback = [](const std::shared_ptr<proto::UnitTest>&) {};

  block_mgr->Publish<proto::UnitTest>("ch1", msgtype);
  block_mgr->Subscribe<proto::UnitTest>("ch1", 10, "cb1", callback);
  auto blocker =
      block_mgr->GetOrCreateBlocker<proto::UnitTest>(BlockerAttr("ch1"));
  EXPECT_NE(blocker, nullptr);
  block_mgr->Unsubscribe<proto::UnitTest>("ch1", "cb1");
  block_mgr->Subscribe<proto::UnitTest>("ch_null", 10, "cb1", callback);
  block_mgr->Observe();
  block_mgr->Reset();
}

}  // namespace blocker
}  // namespace cyber
}  // namespace apollo

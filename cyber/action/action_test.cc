#include "cyber/action/action_context.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "cyber/action/action_types.h"
#include "gtest/gtest.h"

namespace apollo {
namespace cyber {

TEST(ActionContextTest, FeedbackAndCancellation) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  std::string published;
  ActionContext<std::string> context(
      "goal-1", ActionContext<std::string>::Clock::now() +
                    std::chrono::seconds(1),
      cancelled,
      [&published](const std::string& feedback) {
        published = feedback;
        return true;
      });

  EXPECT_EQ(context.goal_id(), "goal-1");
  EXPECT_TRUE(context.PublishFeedback("running"));
  EXPECT_EQ(published, "running");
  EXPECT_GT(context.RemainingTime(), std::chrono::nanoseconds::zero());

  cancelled->store(true);
  EXPECT_TRUE(context.IsCancelled());
  EXPECT_FALSE(context.PublishFeedback("ignored"));
  EXPECT_EQ(published, "running");
}

TEST(ActionOptionsTest, SafeDefaults) {
  ActionOptions options;
  EXPECT_EQ(options.max_active_goals, 1);
  EXPECT_EQ(options.concurrency, 1);
  EXPECT_EQ(options.preemption_policy, ActionPreemptionPolicy::REJECT_NEW);
}

}  // namespace cyber
}  // namespace apollo

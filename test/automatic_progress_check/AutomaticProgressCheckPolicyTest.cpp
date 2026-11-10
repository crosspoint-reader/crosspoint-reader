#include <gtest/gtest.h>

#include <limits>

#include "AutomaticProgressCheckPolicy.h"

TEST(AutomaticProgressCheckPolicy, PromptsOnlyWhenRemoteIsMeaningfullyAhead) {
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.10f, 0.20f), AutomaticProgressDecision::PROMPT);
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.20f, 0.20f), AutomaticProgressDecision::IGNORE);
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.30f, 0.20f), AutomaticProgressDecision::IGNORE);
}

TEST(AutomaticProgressCheckPolicy, IgnoresDifferencesInsideTolerance) {
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.10f, 0.1009f), AutomaticProgressDecision::IGNORE);
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.10f, 0.1011f), AutomaticProgressDecision::PROMPT);
}

TEST(AutomaticProgressCheckPolicy, RejectsInvalidRemotePercentages) {
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.10f, -0.01f), AutomaticProgressDecision::INVALID_REMOTE);
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.10f, 1.01f), AutomaticProgressDecision::INVALID_REMOTE);
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.10f, std::numeric_limits<float>::quiet_NaN()),
            AutomaticProgressDecision::INVALID_REMOTE);
  EXPECT_EQ(AutomaticProgressCheckPolicy::decide(0.10f, std::numeric_limits<float>::infinity()),
            AutomaticProgressDecision::INVALID_REMOTE);
}

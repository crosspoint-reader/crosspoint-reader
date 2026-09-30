#include <gtest/gtest.h>

#include <limits>

#include "AutomaticProgressUploadPolicy.h"

TEST(AutomaticProgressUploadPolicy, UploadsWhenRemoteProgressDoesNotExist) {
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(0.20f, false, 0.0f), AutomaticProgressUploadDecision::UPLOAD);
}

TEST(AutomaticProgressUploadPolicy, UploadsOnlyWhenLocalProgressIsMeaningfullyAhead) {
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(0.20f, true, 0.10f), AutomaticProgressUploadDecision::UPLOAD);
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(0.20f, true, 0.20f), AutomaticProgressUploadDecision::SKIP);
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(0.20f, true, 0.30f), AutomaticProgressUploadDecision::SKIP);
}

TEST(AutomaticProgressUploadPolicy, SkipsDifferencesInsideTolerance) {
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(0.1009f, true, 0.10f), AutomaticProgressUploadDecision::SKIP);
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(0.1011f, true, 0.10f), AutomaticProgressUploadDecision::UPLOAD);
}

TEST(AutomaticProgressUploadPolicy, RejectsInvalidPercentages) {
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(-0.01f, false, 0.0f),
            AutomaticProgressUploadDecision::INVALID_PROGRESS);
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(1.01f, false, 0.0f),
            AutomaticProgressUploadDecision::INVALID_PROGRESS);
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(std::numeric_limits<float>::quiet_NaN(), false, 0.0f),
            AutomaticProgressUploadDecision::INVALID_PROGRESS);
  EXPECT_EQ(AutomaticProgressUploadPolicy::decide(0.20f, true, std::numeric_limits<float>::infinity()),
            AutomaticProgressUploadDecision::INVALID_PROGRESS);
}

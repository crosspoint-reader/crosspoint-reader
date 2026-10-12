#include <gtest/gtest.h>

#include "OpdsFeedError.h"

namespace {

TEST(OpdsFeedError, NoResponseIsUnreachable) {
  EXPECT_EQ(classifyFeedFailure(0), OpdsFeedError::Unreachable);
  EXPECT_EQ(classifyFeedFailure(-1), OpdsFeedError::Unreachable);
}

TEST(OpdsFeedError, OnlyUnauthorizedAsksForCredentials) {
  EXPECT_EQ(classifyFeedFailure(401), OpdsFeedError::Unauthorized);
  // A 403 is often a WAF or Cloudflare block, not a wrong password.
  EXPECT_EQ(classifyFeedFailure(403), OpdsFeedError::Other);
}

TEST(OpdsFeedError, UnauthorizedAfterRedirectIsNotAWrongPassword) {
  // The credentials were withheld from the redirected origin, so say so.
  EXPECT_EQ(classifyFeedFailure(401, true), OpdsFeedError::Redirected);
  EXPECT_EQ(classifyFeedFailure(401, false), OpdsFeedError::Unauthorized);
}

TEST(OpdsFeedError, RedirectOnlyChangesTheUnauthorizedMessage) {
  EXPECT_EQ(classifyFeedFailure(530, true), OpdsFeedError::ServerError);
  EXPECT_EQ(classifyFeedFailure(404, true), OpdsFeedError::Other);
  EXPECT_EQ(classifyFeedFailure(0, true), OpdsFeedError::Unreachable);
}

TEST(OpdsFeedError, EveryFiveHundredIsAServerError) {
  EXPECT_EQ(classifyFeedFailure(500), OpdsFeedError::ServerError);
  EXPECT_EQ(classifyFeedFailure(502), OpdsFeedError::ServerError);
  // Cloudflare answers 530 (error 1033) when the tunnel behind a host is down.
  EXPECT_EQ(classifyFeedFailure(530), OpdsFeedError::ServerError);
  EXPECT_EQ(classifyFeedFailure(599), OpdsFeedError::ServerError);
}

TEST(OpdsFeedError, OtherStatusesKeepTheGenericMessage) {
  EXPECT_EQ(classifyFeedFailure(200), OpdsFeedError::Other);  // body cut short after a good status line
  EXPECT_EQ(classifyFeedFailure(301), OpdsFeedError::Other);
  EXPECT_EQ(classifyFeedFailure(400), OpdsFeedError::Other);
  EXPECT_EQ(classifyFeedFailure(404), OpdsFeedError::Other);
  EXPECT_EQ(classifyFeedFailure(499), OpdsFeedError::Other);
}

}  // namespace

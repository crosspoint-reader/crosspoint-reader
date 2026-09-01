#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "AutomaticWifiConnectionPolicy.h"

TEST(AutomaticWifiConnectionPolicy, KeepsBackgroundConnectionAliveUntilFullDeadline) {
  EXPECT_FALSE(AutomaticWifiConnectionPolicy::backgroundDeadlineExpired(7500, 0));
  EXPECT_FALSE(AutomaticWifiConnectionPolicy::backgroundDeadlineExpired(14999, 0));
  EXPECT_TRUE(AutomaticWifiConnectionPolicy::backgroundDeadlineExpired(15000, 0));
  EXPECT_EQ(AutomaticWifiConnectionPolicy::remainingBackgroundTimeMs(7500, 0), 7500u);
  EXPECT_EQ(AutomaticWifiConnectionPolicy::remainingBackgroundTimeMs(15000, 0), 0u);
}

TEST(AutomaticWifiConnectionPolicy, HandlesMillisRollover) {
  const uint32_t startedAt = std::numeric_limits<uint32_t>::max() - 4999;

  EXPECT_FALSE(AutomaticWifiConnectionPolicy::backgroundDeadlineExpired(9999, startedAt));
  EXPECT_TRUE(AutomaticWifiConnectionPolicy::backgroundDeadlineExpired(10000, startedAt));
  EXPECT_EQ(AutomaticWifiConnectionPolicy::remainingBackgroundTimeMs(9999, startedAt), 1u);
}

TEST(AutomaticWifiConnectionPolicy, ReservesTimeForFallbackNetworks) {
  EXPECT_EQ(AutomaticWifiConnectionPolicy::connectionAttemptTimeoutMs(0, 0, true), 6000u);
  EXPECT_EQ(AutomaticWifiConnectionPolicy::connectionAttemptTimeoutMs(5000, 0, true), 6000u);
  EXPECT_EQ(AutomaticWifiConnectionPolicy::connectionAttemptTimeoutMs(12000, 0, true), 3000u);
}

TEST(AutomaticWifiConnectionPolicy, GivesLastNetworkTheRemainingBudget) {
  EXPECT_EQ(AutomaticWifiConnectionPolicy::connectionAttemptTimeoutMs(0, 0, false), 15000u);
  EXPECT_EQ(AutomaticWifiConnectionPolicy::connectionAttemptTimeoutMs(6000, 0, false), 9000u);
  EXPECT_EQ(AutomaticWifiConnectionPolicy::connectionAttemptTimeoutMs(15000, 0, false), 0u);
}

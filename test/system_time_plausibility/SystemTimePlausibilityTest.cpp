#include "SystemTimePlausibility.h"

#include <gtest/gtest.h>

namespace {

// The condition that decides whether a Wi-Fi connect pays for an SNTP sync.
// Getting it wrong in one direction leaves HTTPS broken after a cold boot; in
// the other it adds a ~5s stall to every connect.

TEST(SystemTimePlausibility, RejectsThePostBootDefault) {
  // ESP-IDF starts at epoch 0 when nothing has set the clock -- the exact case
  // a board with no RTC lands in after every reboot.
  EXPECT_FALSE(isPlausibleEpoch(0));
}

TEST(SystemTimePlausibility, RejectsAnythingBeforeTheThreshold) {
  EXPECT_FALSE(isPlausibleEpoch(1));
  EXPECT_FALSE(isPlausibleEpoch(MIN_PLAUSIBLE_EPOCH - 1));
}

TEST(SystemTimePlausibility, AcceptsTheThresholdItself) {
  EXPECT_TRUE(isPlausibleEpoch(MIN_PLAUSIBLE_EPOCH));
}

TEST(SystemTimePlausibility, AcceptsAnOrdinaryPresentDayReading) {
  // 2026-01-01T00:00:00Z -- a normal synced clock must not trigger a resync.
  EXPECT_TRUE(isPlausibleEpoch(1767225600));
}

TEST(SystemTimePlausibility, ThresholdIsTheDocumentedInstant) {
  // 2024-01-01T00:00:00Z. Pinned so the constant can't drift silently.
  EXPECT_EQ(MIN_PLAUSIBLE_EPOCH, 1704067200);
}

}  // namespace

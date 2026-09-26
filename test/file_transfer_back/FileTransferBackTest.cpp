#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "network/UploadCancel.h"
#include "util/BackTapDetector.h"

namespace {

// Feeds the detector one sample every 5 ms, as the sampler task does, and
// keeps the times it reported a tap.
struct Sampler {
  BackTapDetector detector;
  uint32_t now;
  std::vector<uint32_t> taps;

  explicit Sampler(const uint32_t start = 0) : now(start) {}

  void run(const uint32_t ms, const bool down, const bool valid = true) {
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 5, now += 5) {
      if (detector.update(now, valid, down)) taps.push_back(now);
    }
  }
};

TEST(BackTapDetector, ReportsATapOnceItsReleaseIsStable) {
  Sampler s;
  s.run(100, false);
  s.run(165, true);
  s.run(100, false);
  ASSERT_EQ(s.taps.size(), 1u);
  EXPECT_EQ(s.taps[0], 100u + 165u + BackTapDetector::STABLE_MS);
}

TEST(BackTapDetector, IgnoresAPressShorterThanTheStableWindow) {
  Sampler s;
  s.run(100, false);
  s.run(10, true);
  s.run(100, false);
  EXPECT_TRUE(s.taps.empty());
  s.run(165, true);
  s.run(100, false);
  EXPECT_EQ(s.taps.size(), 1u);
}

TEST(BackTapDetector, IgnoresAButtonHeldWhenSamplingStarts) {
  Sampler s;
  s.run(300, true);
  s.run(100, false);
  EXPECT_TRUE(s.taps.empty());
  s.run(165, true);
  s.run(100, false);
  EXPECT_EQ(s.taps.size(), 1u);
}

TEST(BackTapDetector, DropsInvalidSamples) {
  Sampler s;
  s.run(100, false);
  // Zero on both ladder pins reads as a pressed button; such samples are invalid.
  s.run(30, true, false);
  s.run(100, false);
  EXPECT_TRUE(s.taps.empty());
  // An invalid sample inside a real press does not split it into two.
  s.run(60, true);
  s.run(10, true, false);
  s.run(60, true);
  s.run(100, false);
  EXPECT_EQ(s.taps.size(), 1u);
}

TEST(BackTapDetector, ReportsOnlyTheFirstTap) {
  Sampler s;
  s.run(100, false);
  for (int i = 0; i < 3; ++i) {
    s.run(165, true);
    s.run(200, false);
  }
  EXPECT_EQ(s.taps.size(), 1u);
}

TEST(BackTapDetector, HandlesTheMillisecondClockWrapping) {
  Sampler s(UINT32_MAX - 199);
  s.run(100, false);
  s.run(165, true);
  s.run(100, false);
  EXPECT_EQ(s.taps.size(), 1u);
}

std::vector<int> shut;
void recordShut(const int fd) { shut.push_back(fd); }

class UploadCancelTest : public ::testing::Test {
 protected:
  void SetUp() override { shut.clear(); }
  UploadCancel cancel;
};

TEST_F(UploadCancelTest, ShutsTheUploadBeingRead) {
  cancel.note(7, recordShut);
  EXPECT_TRUE(shut.empty());
  cancel.cancel(recordShut);
  EXPECT_EQ(shut, std::vector<int>{7});
}

TEST_F(UploadCancelTest, ShutsAnUploadStartingAfterTheCancel) {
  cancel.cancel(recordShut);
  EXPECT_TRUE(shut.empty());
  cancel.note(9, recordShut);
  EXPECT_EQ(shut, std::vector<int>{9});
}

TEST_F(UploadCancelTest, LeavesARetractedSocketAlone) {
  cancel.note(7, recordShut);
  cancel.retract();
  cancel.cancel(recordShut);
  EXPECT_TRUE(shut.empty());
}

// A cancel on another task that is shutting the upload's socket holds off the
// main task's retract, so the socket cannot be closed, and its number handed to
// another connection, while it is being shut.
std::atomic<bool> shutting{false};
std::atomic<bool> closed{false};
std::atomic<bool> shutAfterClose{false};
void slowShut(const int) {
  shutting = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (closed) shutAfterClose = true;
}

TEST_F(UploadCancelTest, RetractWaitsForAShutInProgress) {
  cancel.note(7, slowShut);
  std::thread other([this] { cancel.cancel(slowShut); });
  while (!shutting) std::this_thread::yield();
  cancel.retract();
  closed = true;  // the server closes the socket once it has been retracted
  other.join();
  EXPECT_FALSE(shutAfterClose);
}

}  // namespace

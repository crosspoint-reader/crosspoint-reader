#include <gtest/gtest.h>

#include <cstdint>

#include "util/HomeButtonInput.h"
#include "util/PowerButtonInput.h"

TEST(HomeButtonInput, RecognizesConfiguredGestures) {
  using A = HomeButtonAction;
  HomeButtonInput input;
  auto tick = [&](uint32_t now, bool tap = false, bool hold = false, bool swipe = false, bool press = false) {
    return input.update(now, tap, hold, swipe, press, A::Home, A::ToggleFrontlight, A::ReaderMenu);
  };
  EXPECT_EQ(tick(0, true), A::Ignore);
  EXPECT_EQ(tick(350), A::Ignore);
  EXPECT_EQ(tick(351), A::Home);
  EXPECT_EQ(tick(352), A::Ignore);
  EXPECT_EQ(tick(1000, true), A::Ignore);
  EXPECT_EQ(tick(1350, true), A::ToggleFrontlight);
  EXPECT_EQ(tick(1701), A::Ignore);
  EXPECT_EQ(tick(2000, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(2001), A::Ignore);
  // Tap then hold: the second contact prevents an early single-tap action.
  EXPECT_EQ(tick(3000, true), A::Ignore);
  EXPECT_EQ(tick(3200, false, false, false, true), A::Ignore);
  EXPECT_EQ(tick(3400), A::Ignore);
  EXPECT_EQ(tick(3900, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(4000), A::Ignore);
  // A bezel swipe cancels the Home-key event and any deferred action.
  EXPECT_EQ(tick(5000, true), A::Ignore);
  EXPECT_EQ(tick(5100, true, false, true), A::Ignore);
  EXPECT_EQ(tick(5500), A::Ignore);
  EXPECT_EQ(tick(6000, true), A::Ignore);
  input.reset();
  EXPECT_EQ(tick(6500), A::Ignore);
  // Unsigned subtraction remains valid over the millisecond clock wrap.
  EXPECT_EQ(tick(UINT32_MAX - 100, true), A::Ignore);
  EXPECT_EQ(tick(100, true), A::ToggleFrontlight);
  EXPECT_EQ(tick(UINT32_MAX - 100, true), A::Ignore);
  EXPECT_EQ(tick(251), A::Home);
  // Two separated taps each produce a single action.
  EXPECT_EQ(tick(7000, true), A::Ignore);
  EXPECT_EQ(tick(7400, true), A::Home);
  EXPECT_EQ(tick(7751), A::Home);
  // Disabling double tap removes the single-tap delay.
  EXPECT_EQ(input.update(8000, true, false, false, false, A::Bookmark, A::Ignore, A::ReaderMenu), A::Bookmark);
  // Every Home gesture can launch clipping selection without changing persisted action IDs.
  static_assert(static_cast<uint8_t>(A::ToggleFrontlight) == 10);
  EXPECT_EQ(input.update(9000, true, false, false, false, A::CreateClipping, A::Ignore, A::Home), A::CreateClipping);
  EXPECT_EQ(input.update(10000, true, false, false, false, A::Home, A::CreateClipping, A::Ignore), A::Ignore);
  EXPECT_EQ(input.update(10100, true, false, false, false, A::Home, A::CreateClipping, A::Ignore), A::CreateClipping);
  EXPECT_EQ(input.update(11000, false, true, false, false, A::Home, A::Ignore, A::CreateClipping), A::CreateClipping);
}

TEST(HomeButtonInput, PowerWakeHoldAndReleaseAreConsumed) {
  PowerButtonInput input;
  input.begin(true, true);
  for (const uint32_t ms : {0u, 401u, 10000u}) {
    input.update(true, false, ms, 400);
    EXPECT_FALSE(input.shouldSleep());
    EXPECT_FALSE(input.wasShortReleased());
    EXPECT_TRUE(input.suppressesHold());
  }
  input.update(true, true, 10000, 400);  // A new physical press overlaps the committed release.
  EXPECT_FALSE(input.shouldSleep());
  EXPECT_TRUE(input.wasReleaseConsumed());
  EXPECT_FALSE(input.wasShortReleased());
  input.update(true, false, 401, 400);
  EXPECT_TRUE(input.shouldSleep());
  EXPECT_FALSE(input.wasReleaseConsumed());

  // A wake released during setup must not suppress the next complete click.
  input.begin(true, false);
  input.update(false, false, 0, 400);
  input.update(true, false, 0, 400);
  input.update(false, true, 100, 400);
  EXPECT_TRUE(input.wasShortReleased());

  input.begin(true, false);
  input.update(true, false, 0, 400);
  input.update(false, true, 100, 400);
  EXPECT_TRUE(input.wasShortReleased());
  EXPECT_FALSE(input.wasReleaseConsumed());
}

TEST(HomeButtonInput, PowerRestartHoldCanSleepButNeverClick) {
  for (const uint32_t releaseMs : {100u, 400u, 401u, 10000u}) {
    PowerButtonInput input;
    input.begin(false, true);
    input.update(true, false, releaseMs, 400);
    EXPECT_EQ(input.shouldSleep(), releaseMs > 400);
    input.update(true, true, releaseMs, 400);
    EXPECT_FALSE(input.wasShortReleased());
    EXPECT_TRUE(input.wasReleaseConsumed());
    EXPECT_EQ(input.shouldSleep(), releaseMs > 400);
    input.update(true, false, 0, 400);
    input.update(false, true, 100, 400);
    EXPECT_TRUE(input.wasShortReleased());
  }
}

TEST(HomeButtonInput, PowerDurationAndConsumedGesturesHaveOneMeaning) {
  PowerButtonInput input;
  for (const uint16_t threshold : {10, 400}) {
    input.begin(false, false);
    input.update(true, false, threshold, threshold);
    EXPECT_FALSE(input.shouldSleep());
    input.update(true, true, threshold, threshold);
    EXPECT_FALSE(input.shouldSleep());
    EXPECT_TRUE(input.wasShortReleased());
    input.update(false, false, threshold, threshold);
    EXPECT_FALSE(input.wasShortReleased());

    // Release can be the first frame after synchronous work crossed the threshold.
    input.update(true, false, 0, threshold);
    input.update(false, true, 401, threshold);
    EXPECT_FALSE(input.wasShortReleased());
    EXPECT_TRUE(input.wasReleaseConsumed());
    EXPECT_TRUE(input.shouldSleep());
    input.update(false, false, 0, threshold);
    EXPECT_FALSE(input.shouldSleep());

    input.update(true, false, 0, threshold);
    input.update(true, true, 600, threshold);  // The old release wins over a fresh physical press.
    EXPECT_TRUE(input.shouldSleep());
    EXPECT_TRUE(input.wasReleaseConsumed());
    EXPECT_FALSE(input.wasShortReleased());
  }

  // Click-to-sleep must still allow X4 Pro's frontlight double-click window.
  input.begin(false, false);
  input.update(true, false, 20, 10);
  EXPECT_TRUE(input.shouldSleep());
  input.update(false, true, 150, 10);
  EXPECT_TRUE(input.wasShortReleased());

  input.begin(false, false);
  input.update(true, false, 401, 400);
  input.update(false, true, 500, 1000);  // A setting change cannot turn a long hold into a click.
  EXPECT_FALSE(input.shouldSleep());
  EXPECT_FALSE(input.wasShortReleased());

  input.update(true, false, 0, 400);
  input.consume();  // Power+Down or an exit handled on Power press.
  EXPECT_TRUE(input.suppressesHold());
  input.update(true, false, 10000, 400);
  EXPECT_FALSE(input.shouldSleep());
  PowerButtonInput afterRestart;
  afterRestart.begin(input.suppressesHold(), true);
  afterRestart.update(true, false, 10000, 400);
  EXPECT_FALSE(afterRestart.shouldSleep());
  afterRestart.update(false, true, 10000, 400);
  EXPECT_FALSE(afterRestart.shouldSleep());
  EXPECT_TRUE(afterRestart.wasReleaseConsumed());
  EXPECT_FALSE(afterRestart.wasShortReleased());
}

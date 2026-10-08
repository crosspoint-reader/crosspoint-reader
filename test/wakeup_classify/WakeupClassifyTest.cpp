#include <gtest/gtest.h>

#include "WakeupClassify.h"

// The classifier and its pinned IDF constants live in lib/hal/WakeupClassify.h;
// the static_asserts in lib/hal/HalGPIO.cpp verify the pinning against the real
// esp_sleep.h / esp_system.h values at firmware compile time.

namespace {

using wakeup::classify;
using W = wakeup::Reason;

constexpr int UNDEF = wakeup::WAKEUP_UNDEFINED;
constexpr int EXT1 = wakeup::WAKEUP_EXT1;
constexpr int TIMER = wakeup::WAKEUP_TIMER;
constexpr int GPIO = wakeup::WAKEUP_GPIO;
constexpr int RST_UNKNOWN = wakeup::RST_UNKNOWN;
constexpr int RST_POWERON = wakeup::RST_POWERON;
constexpr int RST_SW = wakeup::RST_SW;
constexpr int RST_DEEPSLEEP = wakeup::RST_DEEPSLEEP;

}  // namespace

// 1. A real deep-sleep timer wake classifies as Timer, with or without USB
// plugged in (the deep-sleep evidence must not be overridden by USB state).
TEST(WakeupClassify, TimerWakeClassifiesAsTimer) {
  EXPECT_EQ(classify(TIMER, RST_DEEPSLEEP, false, true), W::Timer);
  EXPECT_EQ(classify(TIMER, RST_DEEPSLEEP, true, true), W::Timer);
  EXPECT_EQ(classify(TIMER, RST_DEEPSLEEP, true, false), W::Timer);
}

// 2. GPIO/ext1 deep-sleep wakes stay PowerButton, USB or not.
TEST(WakeupClassify, DeepSleepGpioWakeStaysPowerButton) {
  EXPECT_EQ(classify(GPIO, RST_DEEPSLEEP, false, true), W::PowerButton);
  EXPECT_EQ(classify(EXT1, RST_DEEPSLEEP, false, true), W::PowerButton);
  EXPECT_EQ(classify(GPIO, RST_DEEPSLEEP, true, false), W::PowerButton);
  EXPECT_EQ(classify(EXT1, RST_DEEPSLEEP, true, false), W::PowerButton);
}

// 3. The timer evidence requires BOTH the deep-sleep reset and the timer wake
// cause; either half alone is not a timer wake.
TEST(WakeupClassify, TimerRequiresDeepSleepResetAndCause) {
  // Sleep-entry abort restarts (RST_SW) and other resets never classify as
  // Timer, even with a stale cause value.
  EXPECT_EQ(classify(TIMER, RST_SW, false, true), W::Other);
  // Deep-sleep exit with an unrecognized cause stays Other.
  EXPECT_EQ(classify(UNDEF, RST_DEEPSLEEP, false, true), W::Other);
  // Other wake sources under a deep-sleep reset stay Other, not Timer.
  EXPECT_EQ(classify(GPIO, RST_DEEPSLEEP, false, false), W::PowerButton);
}

// 4. Existing cold-boot / USB / flash classifications are unchanged.
TEST(WakeupClassify, ColdBootTableUnchanged) {
  // Xteink-style button-latched cold boot without USB = held power button.
  EXPECT_EQ(classify(UNDEF, RST_POWERON, false, true), W::PowerButton);
  // Boards without the button-latch topology keep plain cold boots unclassified.
  EXPECT_EQ(classify(UNDEF, RST_POWERON, false, false), W::Other);
  // USB cold boot and post-flash warm boot.
  EXPECT_EQ(classify(UNDEF, RST_POWERON, true, true), W::AfterUSBPower);
  EXPECT_EQ(classify(UNDEF, RST_UNKNOWN, true, true), W::AfterFlash);
  // Panics/watchdogs/brownouts and causeless unknown resets stay Other.
  EXPECT_EQ(classify(UNDEF, RST_UNKNOWN, false, true), W::Other);
  EXPECT_EQ(classify(UNDEF, RST_SW, false, true), W::Other);
}

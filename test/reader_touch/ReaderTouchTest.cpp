#include <gtest/gtest.h>

#include "activities/reader/ReaderUtils.h"

namespace {
using SwipeDir = MappedInputManager::SwipeDir;

class ReaderTouchTest : public testing::Test {
 protected:
  GfxRenderer renderer;
  MappedInputManager input;

  void SetUp() override {
    SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_TAP_SWIPE;
    gpio.heldMs = 0;
  }

  void tapAt(int x) {
    input.tap = true;
    input.tapX = x;
    input.tapY = 400;
  }

  void expectTurn(bool prev, bool next, unsigned long heldMs = 0) {
    const auto result = ReaderUtils::detectTouchPageTurn(renderer, input);
    EXPECT_EQ(result.prev, prev);
    EXPECT_EQ(result.next, next);
    EXPECT_EQ(result.heldMs, heldMs);
  }
};

TEST_F(ReaderTouchTest, CombinedModeAlternatesTapsAndSwipes) {
  tapAt(20);
  expectTurn(true, false);
  input.tap = false;
  input.swipe = SwipeDir::Left;
  expectTurn(false, true);
  input.swipe = SwipeDir::None;
  gpio.heldMs = ReaderUtils::SKIP_HOLD_MS;
  tapAt(460);
  expectTurn(false, true, gpio.heldMs);
  input.tap = false;
  input.swipe = SwipeDir::Right;
  expectTurn(true, false);
  input.swipe = SwipeDir::None;
  expectTurn(false, false);
  gpio.heldMs = 0;
  tapAt(240);
  expectTurn(false, false);  // The center remains free for the reader menu.
}

TEST_F(ReaderTouchTest, SwipesUseDirectionWithoutPropagatingHoldTime) {
  gpio.heldMs = ReaderUtils::SKIP_HOLD_MS;
  for (const auto direction : {SwipeDir::Left, SwipeDir::Right, SwipeDir::Up, SwipeDir::Down}) {
    SCOPED_TRACE(static_cast<int>(direction));
    input.swipe = direction;
    expectTurn(direction == SwipeDir::Right, direction == SwipeDir::Left);
  }
}

TEST_F(ReaderTouchTest, SwipeOnlyStillIgnoresTaps) {
  SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_SWIPE;
  tapAt(460);
  expectTurn(false, false);
  input.tap = false;
  input.swipe = SwipeDir::Left;
  expectTurn(false, true);
  input.swipe = SwipeDir::Right;
  expectTurn(true, false);
}

TEST_F(ReaderTouchTest, TapModesKeepTheirDirectionsAndIgnoreSwipes) {
  for (const auto mode : {CrossPointSettings::TOUCH_READER_ON, CrossPointSettings::TOUCH_READER_INVERTED_TAP}) {
    SCOPED_TRACE(static_cast<int>(mode));
    SETTINGS.touchReaderControls = mode;
    const bool inverted = mode == CrossPointSettings::TOUCH_READER_INVERTED_TAP;
    input.swipe = SwipeDir::None;
    tapAt(20);
    expectTurn(!inverted, inverted);
    tapAt(460);
    expectTurn(inverted, !inverted);
    input.tap = false;
    input.swipe = SwipeDir::Left;
    expectTurn(false, false);
    input.swipe = SwipeDir::Right;
    expectTurn(false, false);
  }
}
}  // namespace

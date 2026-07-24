#include <gtest/gtest.h>

#include "ReaderUtils.h"

CrossPointSettings testSettings;
HalTiltSensor halTiltSensor;

TEST(ReaderGesture, FiresConfirmHoldAtFiveHundredMillisecondsOnce) {
  ReaderUtils::HoldGestureState state;
  state.onPress();
  EXPECT_FALSE(state.onHold(ReaderUtils::CONFIRM_HOLD_MS - 1, ReaderUtils::CONFIRM_HOLD_MS));
  EXPECT_TRUE(state.onHold(ReaderUtils::CONFIRM_HOLD_MS, ReaderUtils::CONFIRM_HOLD_MS));
  EXPECT_FALSE(state.onHold(ReaderUtils::CONFIRM_HOLD_MS + 1, ReaderUtils::CONFIRM_HOLD_MS));
  EXPECT_EQ(state.onRelease(), ReaderUtils::HoldRelease::Long);
}

TEST(ReaderGesture, FiresLongPageActionOnceAndConsumesRelease) {
  MappedInputManager input;
  ReaderUtils::PageTurnGestureState state;
  testSettings.longPressButtonBehavior = CrossPointSettings::CHAPTER_SKIP;

  input.pressed[static_cast<size_t>(MappedInputManager::Button::Left)] = true;
  input.held[static_cast<size_t>(MappedInputManager::Button::Left)] = true;
  EXPECT_FALSE(ReaderUtils::detectPageTurnGesture(input, state).prev);

  input.clearEdges();
  input.heldTime = ReaderUtils::SKIP_HOLD_MS;
  const auto held = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_TRUE(held.prev);
  EXPECT_TRUE(held.longPress);
  EXPECT_FALSE(ReaderUtils::detectPageTurnGesture(input, state).prev);

  input.held[static_cast<size_t>(MappedInputManager::Button::Left)] = false;
  input.released[static_cast<size_t>(MappedInputManager::Button::Left)] = true;
  EXPECT_TRUE(ReaderUtils::isLongPageTurnRelease(input, state));
  const auto released = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_FALSE(released.prev);
  EXPECT_FALSE(released.next);
}

TEST(ReaderGesture, DefersEnabledShortPageTurnUntilRelease) {
  MappedInputManager input;
  ReaderUtils::PageTurnGestureState state;
  testSettings.longPressButtonBehavior = CrossPointSettings::ORIENTATION_CHANGE;

  input.pressed[static_cast<size_t>(MappedInputManager::Button::Right)] = true;
  input.held[static_cast<size_t>(MappedInputManager::Button::Right)] = true;
  EXPECT_FALSE(ReaderUtils::detectPageTurnGesture(input, state).next);

  input.clearEdges();
  input.held[static_cast<size_t>(MappedInputManager::Button::Right)] = false;
  input.released[static_cast<size_t>(MappedInputManager::Button::Right)] = true;
  const auto released = ReaderUtils::detectPageTurnGesture(input, state);
  EXPECT_TRUE(released.next);
  EXPECT_FALSE(released.longPress);
}

TEST(ReaderGesture, UsesThirtySecondsOnlyWhenNoPreviousAutoTurnIntervalExists) {
  EXPECT_EQ(ReaderUtils::autoPageTurnShortcutSeconds(0), 30);
  EXPECT_EQ(ReaderUtils::autoPageTurnShortcutSeconds(45), 45);
}

TEST(ReaderGesture, ConsumesTheReleaseThatOpenedAReplacementActivity) {
  bool armed = true;
  EXPECT_TRUE(ReaderUtils::consumeInitialRelease(armed, false, true));
  EXPECT_TRUE(armed);
  EXPECT_TRUE(ReaderUtils::consumeInitialRelease(armed, true, false));
  EXPECT_FALSE(armed);
  EXPECT_FALSE(ReaderUtils::consumeInitialRelease(armed, false, false));
}

TEST(ReaderGesture, KeepsBackNavigationBoundaryAtOneSecond) {
  MappedInputManager input;
  ActivityManager activities;
  bool wentHome = false;
  const ReaderUtils::BackNavCallback goHome{&wentHome, [](void* ctx) { *static_cast<bool*>(ctx) = true; }};
  testSettings.backShortToFileBrowser = 0;

  input.held[static_cast<size_t>(MappedInputManager::Button::Back)] = true;
  input.heldTime = ReaderUtils::GO_BACK_OR_HOME_MS;
  EXPECT_TRUE(ReaderUtils::handleBackNavigation(input, activities, "/book.epub", goHome));
  EXPECT_TRUE(activities.openedFileBrowser);
  EXPECT_FALSE(wentHome);

  input.held.fill(false);
  input.released[static_cast<size_t>(MappedInputManager::Button::Back)] = true;
  input.heldTime = ReaderUtils::GO_BACK_OR_HOME_MS - 1;
  EXPECT_TRUE(ReaderUtils::handleBackNavigation(input, activities, "/book.epub", goHome));
  EXPECT_TRUE(wentHome);
}

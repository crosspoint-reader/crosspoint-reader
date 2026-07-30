#include <gtest/gtest.h>

#include "src/util/ButtonShortcutController.h"
#include "src/util/InjectedButtonEvents.h"

using Controller = ButtonShortcutController;

TEST(ButtonShortcutControllerTest, ShortPowerReleaseTogglesQuickLockAndSuppressesInput) {
  Controller controller;

  auto result = controller.update(0U, false, false, true, true, Controller::ChordAction::Disabled);
  EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());

  result = controller.update(0U, false, false, false, true, Controller::ChordAction::Disabled);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());

  result = controller.update(0U, false, false, true, true, Controller::ChordAction::Disabled);
  EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, UnconfiguredPowerReleaseDoesNotLockOrConsume) {
  Controller controller;

  const auto result = controller.update(0U, false, false, true, false, Controller::ChordAction::Disabled);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_FALSE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, ScreenshotChordFiresOnceAndConsumesUntilBothButtonsRelease) {
  Controller controller;

  auto result = controller.update(0U, true, true, false, false, Controller::ChordAction::Screenshot);
  EXPECT_EQ(result.event, Controller::Event::Screenshot);
  EXPECT_TRUE(result.consumeInput);

  result = controller.update(0U, true, true, false, false, Controller::ChordAction::Screenshot);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_TRUE(result.consumeInput);

  result = controller.update(0U, false, true, true, false, Controller::ChordAction::Screenshot);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_TRUE(result.consumeInput);

  result = controller.update(0U, false, false, false, false, Controller::ChordAction::Screenshot);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_TRUE(result.consumeInput);

  result = controller.update(0U, true, true, false, false, Controller::ChordAction::Screenshot);
  EXPECT_EQ(result.event, Controller::Event::Screenshot);
  EXPECT_TRUE(result.consumeInput);
}

TEST(ButtonShortcutControllerTest, QuickLockChordTogglesLockState) {
  Controller controller;

  auto result = controller.update(0U, true, true, false, false, Controller::ChordAction::QuickLock);
  EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
  EXPECT_TRUE(controller.isQuickLocked());

  (void)controller.update(0U, false, false, false, false, Controller::ChordAction::QuickLock);
  result = controller.update(0U, true, true, false, false, Controller::ChordAction::QuickLock);
  EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, PageChordActionsAreReportedWithoutChangingLockState) {
  Controller controller;

  auto result = controller.update(0U, true, true, false, false, Controller::ChordAction::NextPage);
  EXPECT_EQ(result.event, Controller::Event::NextPage);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());

  (void)controller.update(0U, false, false, false, false, Controller::ChordAction::NextPage);
  result = controller.update(0U, true, true, false, false, Controller::ChordAction::PreviousPage);
  EXPECT_EQ(result.event, Controller::Event::PreviousPage);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, DisabledChordDoesNotInterceptButtons) {
  Controller controller;

  const auto result = controller.update(0U, true, true, false, false, Controller::ChordAction::Disabled);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_FALSE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, LockedStateSuppressesNonUnlockChordActions) {
  Controller controller;
  (void)controller.update(0U, false, false, true, true, Controller::ChordAction::Disabled);
  ASSERT_TRUE(controller.isQuickLocked());

  auto result = controller.update(0U, true, true, false, true, Controller::ChordAction::Screenshot);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());

  (void)controller.update(0U, false, false, false, true, Controller::ChordAction::Screenshot);
  result = controller.update(0U, true, true, false, true, Controller::ChordAction::QuickLock);
  EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, RestoredQuickLockStartsANewTimeoutWindow) {
  Controller controller;

  controller.restoreQuickLock(1000U);

  EXPECT_TRUE(controller.isQuickLocked());
  EXPECT_FALSE(controller.shouldQuickLockSleep(5999U, 5000U));
  EXPECT_TRUE(controller.shouldQuickLockSleep(6000U, 5000U));
}

TEST(ButtonShortcutControllerTest, LockTimestampFeedsAutoSleep) {
  Controller controller;

  (void)controller.update(1000U, false, false, true, true, Controller::ChordAction::Disabled);

  EXPECT_FALSE(controller.shouldQuickLockSleep(5999U, 5000U));
  EXPECT_TRUE(controller.shouldQuickLockSleep(6000U, 5000U));
}

TEST(InjectedButtonEventsTest, ClickProvidesBothEdgesForOneFrame) {
  InjectedButtonEvents events;
  constexpr uint8_t pageForward = 8;

  events.injectClick(pageForward);
  EXPECT_TRUE(events.wasPressed(pageForward));
  EXPECT_TRUE(events.wasReleased(pageForward));
  EXPECT_TRUE(events.wasPressed(pageForward));

  events.clear();
  EXPECT_FALSE(events.wasPressed(pageForward));
  EXPECT_FALSE(events.wasReleased(pageForward));
}

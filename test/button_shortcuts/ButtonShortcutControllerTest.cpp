#include <gtest/gtest.h>

#include "src/util/ButtonShortcutController.h"

using Controller = ButtonShortcutController;

TEST(ButtonShortcutControllerTest, ShortPowerReleaseLocksAndSuppressesInput) {
  Controller controller;

  auto result = controller.update(0U, false, false, true, true, Controller::ChordAction::Disabled);
  EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());

  result = controller.update(0U, false, false, false, true, Controller::ChordAction::Disabled);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, ShortPowerReleaseDoesNotUnlock) {
  Controller controller;
  (void)controller.update(0U, false, false, true, true, Controller::ChordAction::Disabled);
  ASSERT_TRUE(controller.isQuickLocked());

  const auto result = controller.update(0U, false, false, true, true, Controller::ChordAction::Disabled);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());
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

TEST(ButtonShortcutControllerTest, ChordUnlocksWhateverItIsSetTo) {
  for (const auto action : {Controller::ChordAction::Screenshot, Controller::ChordAction::NextPage,
                            Controller::ChordAction::PreviousPage, Controller::ChordAction::Disabled}) {
    Controller controller;
    (void)controller.update(0U, false, false, true, true, action);
    ASSERT_TRUE(controller.isQuickLocked());

    const auto result = controller.update(0U, true, true, false, true, action);
    EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
    EXPECT_TRUE(result.consumeInput);
    EXPECT_FALSE(controller.isQuickLocked());
  }
}

TEST(ButtonShortcutControllerTest, ChordReleaseAfterUnlockDoesNotRelock) {
  Controller controller;
  (void)controller.update(0U, false, false, true, true, Controller::ChordAction::Disabled);
  (void)controller.update(0U, true, true, false, true, Controller::ChordAction::Disabled);
  ASSERT_FALSE(controller.isQuickLocked());

  auto result = controller.update(0U, false, true, true, true, Controller::ChordAction::Disabled);
  EXPECT_TRUE(result.consumeInput);
  result = controller.update(0U, false, false, false, true, Controller::ChordAction::Disabled);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, LockStartsANewTimeoutWindow) {
  Controller controller;

  controller.lock(1000U);

  EXPECT_TRUE(controller.isQuickLocked());
  EXPECT_FALSE(controller.shouldQuickLockSleep(5999U, 5000U));
  EXPECT_TRUE(controller.shouldQuickLockSleep(6000U, 5000U));
}

TEST(ButtonShortcutControllerTest, ActivityWhileLockedRestartsTimeout) {
  Controller controller;
  controller.lock(1000U);

  controller.noteActivity(4000U);

  EXPECT_FALSE(controller.shouldQuickLockSleep(8999U, 5000U));
  EXPECT_TRUE(controller.shouldQuickLockSleep(9000U, 5000U));
}

TEST(ButtonShortcutControllerTest, LockTimestampFeedsAutoSleep) {
  Controller controller;

  (void)controller.update(1000U, false, false, true, true, Controller::ChordAction::Disabled);

  EXPECT_FALSE(controller.shouldQuickLockSleep(5999U, 5000U));
  EXPECT_TRUE(controller.shouldQuickLockSleep(6000U, 5000U));
}

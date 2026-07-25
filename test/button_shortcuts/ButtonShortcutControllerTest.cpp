#include <gtest/gtest.h>

#include "src/util/ButtonShortcutController.h"

using Controller = ButtonShortcutController;

TEST(ButtonShortcutControllerTest, ShortPowerReleaseTogglesQuickLockAndSuppressesInput) {
  Controller controller;

  auto result = controller.update(true, true);
  EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());

  result = controller.update(false, true);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());

  result = controller.update(true, true);
  EXPECT_EQ(result.event, Controller::Event::QuickLockChanged);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutControllerTest, UnconfiguredPowerReleaseDoesNotLockOrConsume) {
  Controller controller;

  const auto result = controller.update(true, false);
  EXPECT_EQ(result.event, Controller::Event::None);
  EXPECT_FALSE(result.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
}

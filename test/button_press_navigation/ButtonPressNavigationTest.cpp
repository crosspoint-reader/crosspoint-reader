#include <gtest/gtest.h>

#include "util/ButtonNavigator.h"

unsigned long testNowMs = 1000;

namespace {
constexpr uint8_t PREVIOUS = 1;
constexpr uint8_t NEXT = 2;

class ButtonPressNavigationTest : public ::testing::Test {
 protected:
  MappedInputManager input;
  ButtonNavigator navigator;
  int selected = 0;
  int pages = 0;

  void SetUp() override {
    testNowMs = 1000;
    ButtonNavigator::setMappedInputManager(input);
  }

  // The shared list call order, with the production navigator.
  void navigate() {
    navigator.onNextPress([this] { selected = ButtonNavigator::nextIndex(selected, 10); });
    navigator.onPreviousPress([this] { selected = ButtonNavigator::previousIndex(selected, 10); });
    navigator.onNextContinuous([this] { ++pages; });
    navigator.onPreviousContinuous([this] { --pages; });
  }
};

TEST_F(ButtonPressNavigationTest, PressStepsImmediatelyAndReleaseDoesNotStep) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  input.frame = {90, 0, NEXT, 0};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 0);
}

TEST_F(ButtonPressNavigationTest, HoldPagesAfterThresholdAndStopsOnRelease) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  testNowMs += 501;
  input.frame = {501, 0, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 1);
  testNowMs += 100;
  input.frame.heldMs += 100;
  navigate();
  EXPECT_EQ(pages, 1);
  testNowMs += 1000;
  input.frame = {1601, 0, NEXT, 0};
  navigate();
  EXPECT_EQ(pages, 1);
}

TEST_F(ButtonPressNavigationTest, PressNeverAlsoRepeatsEvenWithOldHeldDuration) {
  input.frame = {2000, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 0);
}

TEST_F(ButtonPressNavigationTest, FastDirectionChangeStartsANewRepeatInterval) {
  input.frame = {501, 0, 0, NEXT};
  navigate();
  EXPECT_EQ(pages, 1);
  input.frame = {0, PREVIOUS, NEXT, PREVIOUS};
  navigate();
  EXPECT_EQ(selected, 9);
  EXPECT_EQ(pages, 1);
}

}  // namespace

#include <gtest/gtest.h>

#include "util/ButtonInputBuffer.h"
#include "util/ButtonNavigator.h"

unsigned long testNowMs = 1000;

namespace {
constexpr uint8_t PREVIOUS = 1;
constexpr uint8_t NEXT = 2;
constexpr uint8_t CONFIRM = 4;
constexpr uint8_t NAVIGATION = PREVIOUS | NEXT;

class ButtonNavigationTest : public ::testing::Test {
 protected:
  MappedInputManager input;
  ButtonNavigator navigator;
  ButtonInputBuffer buffer;
  int selected = 0;
  int pages = 0;

  void SetUp() override {
    testNowMs = 1000;
    ButtonNavigator::setMappedInputManager(input);
  }

  // The shared list call order, with the production navigator and input queue.
  void navigate() {
    navigator.onNextPress([this] { selected = ButtonNavigator::nextIndex(selected, 10); });
    navigator.onPreviousPress([this] { selected = ButtonNavigator::previousIndex(selected, 10); });
    navigator.onNextContinuous([this] { ++pages; });
    navigator.onPreviousContinuous([this] { --pages; });
  }
};

TEST_F(ButtonNavigationTest, BufferedListStepsOnPress) {
  input.frame = {0, NEXT, 0, NEXT};
  navigate();
  EXPECT_EQ(selected, 1);
  input.frame = {90, 0, NEXT, 0};
  navigate();
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(pages, 0);
}

TEST_F(ButtonNavigationTest, HoldPagesAfterThresholdAndStopsOnRelease) {
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

TEST_F(ButtonNavigationTest, BurstDuringRenderPreservesOrderAndFinalSelection) {
  for (const uint8_t button : {NEXT, NEXT, PREVIOUS, NEXT}) {
    ASSERT_TRUE(buffer.capture({0, button, 0, button}));
    ASSERT_TRUE(buffer.capture({60, 0, button, 0}));
  }
  EXPECT_EQ(selected, 0);  // Rendering still owns the activity.
  int events = 0;
  while (buffer.pop(input.frame)) {
    navigate();
    ++events;
  }
  EXPECT_EQ(events, 8);
  EXPECT_EQ(selected, 2);
  EXPECT_EQ(pages, 0);
}

TEST_F(ButtonNavigationTest, ConfirmIsAnOrderedBoundaryBetweenNavigationBursts) {
  ASSERT_TRUE(buffer.capture({0, NEXT, 0, NEXT}));
  ASSERT_TRUE(buffer.capture({70, 0, NEXT, 0}));
  ASSERT_TRUE(buffer.capture({0, CONFIRM, 0, CONFIRM}));
  ASSERT_TRUE(buffer.capture({70, 0, CONFIRM, 0}));
  ASSERT_TRUE(buffer.capture({0, NEXT, 0, NEXT}));
  while (buffer.front() && buffer.front()->navigationOnly(NAVIGATION)) {
    ASSERT_TRUE(buffer.pop(input.frame));
    navigate();
  }
  EXPECT_EQ(selected, 1);
  ASSERT_NE(buffer.front(), nullptr);
  EXPECT_EQ(buffer.front()->pressed, CONFIRM);
  buffer.reset(CONFIRM);  // Activation changes activity: discard the remaining burst.
  EXPECT_EQ(buffer.front(), nullptr);
}

TEST_F(ButtonNavigationTest, HeldActivationButtonPreventsCoalescingDirections) {
  EXPECT_FALSE((ButtonInputBuffer::Frame{20, NEXT, 0, NEXT | CONFIRM}).navigationOnly(NAVIGATION));
}

TEST_F(ButtonNavigationTest, RemappedConfirmIsNotTreatedAsNavigation) {
  constexpr uint8_t remappedNavigation = PREVIOUS | CONFIRM;
  EXPECT_FALSE((ButtonInputBuffer::Frame{0, NEXT, 0, NEXT}).navigationOnly(remappedNavigation));
  EXPECT_TRUE((ButtonInputBuffer::Frame{0, CONFIRM, 0, CONFIRM}).navigationOnly(remappedNavigation));
}

TEST_F(ButtonNavigationTest, TransitionSuppressesHeldButtonUntilItsRelease) {
  buffer.reset(NEXT);
  ASSERT_TRUE(buffer.capture({100, 0, 0, NEXT}));
  EXPECT_EQ(buffer.current().held, 0);
  ASSERT_TRUE(buffer.capture({200, 0, NEXT, 0}));
  EXPECT_EQ(buffer.front(), nullptr);
  EXPECT_EQ(buffer.blocked(), NEXT);
  ASSERT_TRUE(buffer.capture({0, NEXT, 0, NEXT}));
  ASSERT_TRUE(buffer.pop(input.frame));
  navigate();
  EXPECT_EQ(selected, 1);
  ASSERT_TRUE(buffer.capture({60, 0, NEXT, 0}));
  ASSERT_TRUE(buffer.pop(input.frame));
  navigate();
  EXPECT_EQ(selected, 1);
}

TEST_F(ButtonNavigationTest, QueueRetainsReleaseDurationForDelayedLongPress) {
  ASSERT_TRUE(buffer.capture({0, CONFIRM, 0, CONFIRM}));
  ASSERT_TRUE(buffer.capture({1200, 0, CONFIRM, 0}));
  ASSERT_TRUE(buffer.pop(input.frame));
  EXPECT_EQ(input.getHeldTime(), 0u);
  ASSERT_TRUE(buffer.pop(input.frame));
  EXPECT_EQ(input.getHeldTime(), 1200u);
  EXPECT_EQ(input.frame.released, CONFIRM);
}

TEST_F(ButtonNavigationTest, SynchronousInputPumpDiscardsPendingMenuClicks) {
  ASSERT_TRUE(buffer.capture({0, NEXT, 0, NEXT}));
  ASSERT_TRUE(buffer.capture({70, 0, NEXT, 0}));
  ASSERT_TRUE(buffer.capture({60, 0, CONFIRM, 0}, false));
  EXPECT_EQ(buffer.front(), nullptr);
  EXPECT_EQ(buffer.current().released, CONFIRM);
  ASSERT_TRUE(buffer.capture({0, NEXT, 0, NEXT}));
  ASSERT_TRUE(buffer.pop(input.frame));
  navigate();
  EXPECT_EQ(selected, 1);
  ASSERT_TRUE(buffer.capture({60, 0, NEXT, 0}));
  ASSERT_TRUE(buffer.pop(input.frame));
  navigate();
  EXPECT_EQ(selected, 1);
}

TEST_F(ButtonNavigationTest, OverflowCannotActivateFromAnIncompleteHistory) {
  for (uint8_t i = 0; i < ButtonInputBuffer::CAPACITY; ++i) {
    ASSERT_TRUE(buffer.capture({0, NEXT, 0, NEXT}));
  }
  EXPECT_FALSE(buffer.capture({0, CONFIRM, 0, CONFIRM}));
  EXPECT_TRUE(buffer.dropping());
  EXPECT_EQ(buffer.front(), nullptr);
  EXPECT_EQ(buffer.current().pressed, 0);
  ASSERT_TRUE(buffer.capture({60, 0, CONFIRM, 0}));
  EXPECT_FALSE(buffer.dropping());
  EXPECT_EQ(buffer.current().released, 0);
  EXPECT_EQ(buffer.front(), nullptr);
  ASSERT_TRUE(buffer.capture({0, NEXT, 0, NEXT}));
  ASSERT_TRUE(buffer.pop(input.frame));
  navigate();
  EXPECT_EQ(selected, 1);
  ASSERT_TRUE(buffer.capture({60, 0, NEXT, 0}));
  ASSERT_TRUE(buffer.pop(input.frame));
  navigate();
  EXPECT_EQ(selected, 1);
}

TEST_F(ButtonNavigationTest, RingWrapKeepsEveryEdge) {
  for (unsigned i = 0; i < 100; ++i) {
    ASSERT_TRUE(buffer.capture({0, NEXT, 0, NEXT}));
    ASSERT_TRUE(buffer.pop(input.frame));
    navigate();
    ASSERT_TRUE(buffer.capture({20, 0, NEXT, 0}));
    ASSERT_TRUE(buffer.pop(input.frame));
    navigate();
  }
  EXPECT_EQ(selected, 0);
  EXPECT_EQ(buffer.front(), nullptr);
}

TEST_F(ButtonNavigationTest, DelayedHoldReleaseDoesNotTriggerShortReleaseAction) {
  input.frame = {1500, 0, PREVIOUS, 0};
  bool activated = false;
  navigator.onPreviousRelease([&activated] { activated = true; });
  EXPECT_FALSE(activated);
}

static_assert(sizeof(ButtonInputBuffer) <= 280, "Button buffering must stay bounded on C3");
}  // namespace

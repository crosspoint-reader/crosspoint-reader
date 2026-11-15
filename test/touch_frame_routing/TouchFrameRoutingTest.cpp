#include <gtest/gtest.h>

#include <initializer_list>

#include "components/TouchFrameRouting.h"

namespace fui = freeink::ui;

namespace {

constexpr fui::ActionId kSlider = 1;
constexpr fui::ActionId kButton = 2;
constexpr int16_t kY = 20;

// Slider track spans x = 0..200 px.
struct SliderScreen {
  fui::InteractionBuffer<4> buffer;

  SliderScreen() {
    fui::Interaction slider;
    slider.rect = {0, 0, 201, 40};
    slider.action = kSlider;
    slider.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputDrag);
    buffer.addInteraction(slider);
  }

  // Held frame without a press: live position only.
  fui::ActionEvent held(const int16_t liveX) {
    fui::InputSnapshot snap{};
    snap.touchHeld = true;
    snap.touchX = liveX;
    snap.touchY = kY;
    return route(snap, liveX);
  }

  // Tap-candidate frame: the adapter repeats the press at the landing point.
  fui::ActionEvent pressedAndHeld(const int16_t landingX, const int16_t liveX) {
    fui::InputSnapshot snap{};
    snap.touchHeld = true;
    snap.touchPressed = true;
    snap.touchX = landingX;
    snap.touchY = kY;
    return route(snap, liveX);
  }

  fui::ActionEvent route(const fui::InputSnapshot& snap, const int16_t liveX) {
    return routePressThenHold(snap, liveX, kY, buffer.activeIndex() >= 0,
                              [this](const fui::InputSnapshot& s) { return buffer.route(s); });
  }
};

// Follows the InputManager::suppressTouchContact() contract: once suppressed,
// the contact reports nothing (hold, release, swipe) until the finger lifts.
struct FakeTouch {
  bool down = false;
  mutable bool suppressed = false;

  bool isScreenTouchHeld(int& x, int& y) const {
    if (!down || suppressed) return false;
    x = 0;
    y = kY;
    return true;
  }
  void suppressScreenTouch() const { suppressed = down; }
};

// One routeHeld pass of UiAppHost::routeTouch; `snap` is what the frame would
// report without suppression.
fui::ActionEvent pass(SliderScreen& screen, const FakeTouch& touch, const bool ready, const fui::InputSnapshot& snap,
                      const int16_t liveX) {
  if (!ready) {
    suppressContactBeforeReady(touch);
    return {};
  }
  if (touch.suppressed) return {};
  return screen.route(snap, liveX);
}

fui::InputSnapshot heldSnap(const int16_t liveX) {
  fui::InputSnapshot snap{};
  snap.touchHeld = true;
  snap.touchX = liveX;
  snap.touchY = kY;
  return snap;
}

fui::InputSnapshot pressedSnap(const int16_t landingX) {
  fui::InputSnapshot snap = heldSnap(landingX);
  snap.touchPressed = true;
  return snap;
}

// x = landing point for a tap, -1 for a drag-off release.
fui::InputSnapshot releaseSnap(const int16_t x) {
  fui::InputSnapshot snap{};
  snap.touchReleased = true;
  snap.touchX = x;
  snap.touchY = x < 0 ? -1 : kY;
  return snap;
}

}  // namespace

TEST(TouchFrameRouting, DragFollowsLivePositionDuringTapSlop) {
  SliderScreen screen;
  EXPECT_EQ(screen.held(100).dragPermille, 500);

  const auto event = screen.pressedAndHeld(100, 115);
  EXPECT_EQ(event.action, kSlider);
  EXPECT_EQ(event.dragPermille, 575);
}

TEST(TouchFrameRouting, DragStartedInsideSurvivesLeavingTrackDuringTapSlop) {
  SliderScreen screen;
  EXPECT_EQ(screen.held(195).action, kSlider);

  const auto inSlop = screen.pressedAndHeld(195, 208);
  EXPECT_EQ(inSlop.action, kSlider);
  EXPECT_EQ(inSlop.dragPermille, 1000);

  const auto later = screen.held(245);
  EXPECT_EQ(later.action, kSlider);
  EXPECT_EQ(later.dragPermille, 1000);
}

TEST(TouchFrameRouting, ContactStartedOutsideDoesNotGrabSliderDuringTapSlop) {
  SliderScreen screen;
  EXPECT_EQ(screen.held(205).action, fui::NO_ACTION);

  EXPECT_EQ(screen.pressedAndHeld(205, 195).action, fui::NO_ACTION);
  EXPECT_EQ(screen.buffer.activeIndex(), -1);
  EXPECT_EQ(screen.held(150).action, fui::NO_ACTION);
}

// The router's first frame of a contact can already carry the delayed press,
// e.g. when no loop pass saw the contact before routing opened.
TEST(TouchFrameRouting, FirstFrameInsideToOutside) {
  SliderScreen screen;
  const auto first = screen.pressedAndHeld(195, 208);
  EXPECT_EQ(first.action, kSlider);
  EXPECT_EQ(first.dragPermille, 975);

  const auto later = screen.held(245);
  EXPECT_EQ(later.action, kSlider);
  EXPECT_EQ(later.dragPermille, 1000);
}

TEST(TouchFrameRouting, FirstFrameOutsideToInside) {
  SliderScreen screen;
  EXPECT_EQ(screen.pressedAndHeld(205, 195).action, fui::NO_ACTION);
  EXPECT_EQ(screen.buffer.activeIndex(), -1);
  EXPECT_EQ(screen.held(150).action, fui::NO_ACTION);
}

TEST(TouchFrameRouting, PressHighlightStaysOnLandingElement) {
  fui::InteractionBuffer<4> buffer;
  for (int16_t i = 0; i < 2; ++i) {
    fui::Interaction button;
    button.rect = {static_cast<int16_t>(i * 100), 0, 100, 40};
    button.action = kButton;
    button.value = i;
    buffer.addInteraction(button);
  }
  const auto route = [&buffer](const fui::InputSnapshot& s) { return buffer.route(s); };

  fui::InputSnapshot snap{};
  snap.touchHeld = true;
  snap.touchX = 95;
  snap.touchY = kY;
  EXPECT_EQ(routePressThenHold(snap, 95, kY, buffer.activeIndex() >= 0, route).action, fui::NO_ACTION);

  snap.touchPressed = true;
  EXPECT_EQ(routePressThenHold(snap, 110, kY, buffer.activeIndex() >= 0, route).action, fui::NO_ACTION);
  EXPECT_EQ(buffer.activeIndex(), 0);
}

TEST(TouchFrameRouting, SnapshotWithoutPressAndHoldRoutesUnchanged) {
  int calls = 0;
  fui::InputSnapshot seen{};
  const auto route = [&](const fui::InputSnapshot& s) {
    ++calls;
    seen = s;
    return fui::ActionEvent{};
  };

  fui::InputSnapshot release{};
  release.touchReleased = true;
  release.touchX = 42;
  release.touchY = kY;
  routePressThenHold(release, 99, kY, /*touchBound=*/true, route);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(seen.touchReleased);
  EXPECT_EQ(seen.touchX, 42);
}

// Finger lands outside the slider while the screen renders; routing opens
// before the press delay, then the finger moves onto the slider and lifts.
TEST(TouchFrameRouting, ContactFromBeforeRoutingOpenedIsIgnoredBeforePressDelay) {
  SliderScreen screen;
  FakeTouch touch;
  touch.down = true;
  EXPECT_EQ(pass(screen, touch, false, heldSnap(205), 205).action, fui::NO_ACTION);
  EXPECT_TRUE(touch.suppressed);

  EXPECT_EQ(pass(screen, touch, true, heldSnap(198), 198).action, fui::NO_ACTION);
  EXPECT_EQ(pass(screen, touch, true, pressedSnap(205), 195).action, fui::NO_ACTION);
  EXPECT_EQ(pass(screen, touch, true, heldSnap(150), 150).action, fui::NO_ACTION);
  touch.down = false;
  EXPECT_EQ(pass(screen, touch, true, releaseSnap(-1), 0).action, fui::NO_ACTION);
  EXPECT_EQ(screen.buffer.activeIndex(), -1);
}

// Routing opens after the finger has moved past tap slop: only held frames at
// the live position, then a drag-off release.
TEST(TouchFrameRouting, ContactFromBeforeRoutingOpenedIsIgnoredAfterTapSlop) {
  SliderScreen screen;
  FakeTouch touch;
  touch.down = true;
  EXPECT_EQ(pass(screen, touch, false, heldSnap(205), 205).action, fui::NO_ACTION);
  EXPECT_EQ(pass(screen, touch, false, heldSnap(230), 230).action, fui::NO_ACTION);

  EXPECT_EQ(pass(screen, touch, true, heldSnap(195), 195).action, fui::NO_ACTION);
  EXPECT_EQ(pass(screen, touch, true, heldSnap(150), 150).action, fui::NO_ACTION);
  touch.down = false;
  EXPECT_EQ(pass(screen, touch, true, releaseSnap(-1), 0).action, fui::NO_ACTION);
  EXPECT_EQ(screen.buffer.activeIndex(), -1);
}

// A tap on the slider jumps it to the tap point, so a contact that landed on
// it before routing opened must not reach the router on release either.
TEST(TouchFrameRouting, ReleaseOfContactFromBeforeRoutingOpenedIsIgnored) {
  SliderScreen screen;
  FakeTouch touch;
  touch.down = true;
  EXPECT_EQ(pass(screen, touch, false, heldSnap(150), 150).action, fui::NO_ACTION);
  EXPECT_EQ(pass(screen, touch, true, pressedSnap(150), 150).action, fui::NO_ACTION);
  touch.down = false;
  EXPECT_EQ(pass(screen, touch, true, releaseSnap(150), 0).action, fui::NO_ACTION);
}

TEST(TouchFrameRouting, NothingSuppressedWithoutContactBeforeRoutingOpened) {
  FakeTouch touch;
  suppressContactBeforeReady(touch);
  EXPECT_FALSE(touch.suppressed);
}

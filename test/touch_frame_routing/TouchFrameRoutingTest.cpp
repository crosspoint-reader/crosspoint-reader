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
    return routePressThenHold(snap, liveX, kY, [this](const fui::InputSnapshot& s) { return buffer.route(s); });
  }
};

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
  EXPECT_EQ(routePressThenHold(snap, 95, kY, route).action, fui::NO_ACTION);

  snap.touchPressed = true;
  EXPECT_EQ(routePressThenHold(snap, 110, kY, route).action, fui::NO_ACTION);
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
  routePressThenHold(release, 99, kY, route);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(seen.touchReleased);
  EXPECT_EQ(seen.touchX, 42);
}

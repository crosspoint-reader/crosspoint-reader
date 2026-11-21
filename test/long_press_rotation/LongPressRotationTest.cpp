// Long-press orientation change. Without the orientation swap, next page turns right
// (index - 1); with it, a held button keeps turning the same way in every orientation.
// The model mirrors MappedInputManager's page-button mapping.
#include <gtest/gtest.h>

#include "src/activities/reader/LongPressRotation.h"

namespace {
using longpressrotation::heldButtonSwapped;
using longpressrotation::nextOrientation;
using longpressrotation::turnsRight;

constexpr uint8_t COUNT = 4;  // 0 Portrait, 1 Landscape CW, 2 Inverted, 3 Landscape CCW
enum class Held { SideUp, SideDown, FrontLeft, FrontRight };
enum class Layout { PrevNext, NextPrev, PrevPrev, NextNext };
constexpr Held ALL_BUTTONS[] = {Held::SideUp, Held::SideDown, Held::FrontLeft, Held::FrontRight};
constexpr Layout ALL_LAYOUTS[] = {Layout::PrevNext, Layout::NextPrev, Layout::PrevPrev, Layout::NextNext};

bool navSwapped(const uint8_t orientation, const bool swapPolicy) {
  return swapPolicy && (orientation == 2 || orientation == 3);
}

bool isFront(const Held held) { return held == Held::FrontLeft || held == Held::FrontRight; }
bool sideFollows(const Layout layout) { return layout == Layout::PrevNext || layout == Layout::NextPrev; }

// Does holding this button read as "next page" right now?
bool isNext(const Held held, const Layout layout, const uint8_t orientation, const bool swapPolicy) {
  const bool swap = navSwapped(orientation, swapPolicy);
  if (isFront(held)) return (held == Held::FrontRight) != swap;
  if (layout == Layout::NextNext) return true;
  if (layout == Layout::PrevPrev) return false;
  const bool downIsNext = layout == Layout::PrevNext;
  return ((held == Held::SideDown) == downIsNext) != swap;
}

uint8_t hold(const Held held, const Layout layout, const uint8_t orientation, const bool swapPolicy) {
  const bool next = isNext(held, layout, orientation, swapPolicy);
  const bool swapped =
      heldButtonSwapped(navSwapped(orientation, swapPolicy), isFront(held), !isFront(held), sideFollows(layout));
  return nextOrientation(orientation, turnsRight(next, swapped), COUNT);
}

uint8_t nextTurnsRight(const bool next, const uint8_t orientation) {
  return next ? (orientation - 1 + COUNT) % COUNT : (orientation + 1) % COUNT;
}
}  // namespace

TEST(LongPressRotation, WithoutSwapNextPageTurnsRight) {
  for (const Layout layout : ALL_LAYOUTS) {
    for (const Held held : ALL_BUTTONS) {
      for (uint8_t o = 0; o < COUNT; o++) {
        EXPECT_EQ(hold(held, layout, o, false), nextTurnsRight(isNext(held, layout, o, false), o));
      }
    }
  }
}

TEST(LongPressRotation, WithSwapTheSameButtonAlwaysTurnsTheSameWay) {
  for (const Layout layout : ALL_LAYOUTS) {
    for (const Held held : ALL_BUTTONS) {
      int direction = 0;
      for (uint8_t o = 0; o < COUNT; o++) {
        const int step = (hold(held, layout, o, true) - o + COUNT) % COUNT == 1 ? 1 : -1;
        if (direction == 0) direction = step;
        EXPECT_EQ(step, direction) << "layout " << int(layout) << " button " << int(held) << " orientation " << int(o);
      }
    }
  }
}

TEST(LongPressRotation, WithSwapRepeatedHoldsWalkAllOrientations) {
  for (const Layout layout : ALL_LAYOUTS) {
    for (const Held held : ALL_BUTTONS) {
      uint8_t o = 2;
      bool seen[COUNT] = {};
      for (int i = 0; i < COUNT; i++) {
        o = hold(held, layout, o, true);
        seen[o] = true;
      }
      EXPECT_EQ(o, 2) << "layout " << int(layout) << " button " << int(held);
      for (const bool s : seen) EXPECT_TRUE(s);
    }
  }
}

TEST(LongPressRotation, WithSwapNextPageTurnsRightWhereNothingIsSwapped) {
  for (const Layout layout : ALL_LAYOUTS) {
    for (const Held held : ALL_BUTTONS) {
      for (const uint8_t o : {0, 1}) {
        EXPECT_EQ(hold(held, layout, o, true), nextTurnsRight(isNext(held, layout, o, true), o));
      }
    }
  }
}

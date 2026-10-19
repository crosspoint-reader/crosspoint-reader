// LaneBeforeBuild: who has the secondary framebuffer when an image decode and Background-B's build both want it.
#include <gtest/gtest.h>

#include "activities/reader/LaneBeforeBuild.h"

namespace {

using LaneBeforeBuild::buildMayBorrow;
using LaneBeforeBuild::Decode;
using LaneBeforeBuild::decodeWhileBuilding;
using LaneBeforeBuild::kMaxYields;
using LaneBeforeBuild::Window;

TEST(LaneBeforeBuild, TheBuildTakesTheBufferOnlyOnceTheLaneIsDoneWithItsWindow) {
  EXPECT_TRUE(buildMayBorrow(Window::Clean));     // looked at, nothing to decode
  EXPECT_TRUE(buildMayBorrow(Window::Closed));    // the lane cannot run: B decides by its own gates
  EXPECT_FALSE(buildMayBorrow(Window::Unknown));  // not looked at yet: the pictures ahead may need the buffer
  EXPECT_FALSE(buildMayBorrow(Window::Dirty));    // the lane is decoding them
}

TEST(LaneBeforeBuild, ADecodeTakesTheBufferFromTheBuildWhileItHasYieldsLeft) {
  for (uint8_t yields = 0; yields < kMaxYields; ++yields) {
    EXPECT_EQ(Decode::TakeFromBuild, decodeWhileBuilding(yields)) << static_cast<int>(yields);
  }
}

TEST(LaneBeforeBuild, PastItsYieldsTheBuildKeepsTheBuffer) {
  EXPECT_EQ(Decode::AfterBuild, decodeWhileBuilding(kMaxYields));
  EXPECT_EQ(Decode::AfterBuild, decodeWhileBuilding(255));
}

TEST(LaneBeforeBuild, EveryPageBuiltThroughGetsItsYield) {
  // Building through the last three pages (BG_BUILD_THROUGH_PAGES), plus the page a build that began earlier kept the
  // buffer on: a picture on each of them is decoded in the buffer, none on the heap or as a placeholder.
  constexpr uint8_t kBuildThroughPages = 3;
  EXPECT_GE(kMaxYields, kBuildThroughPages + 1);
}

TEST(LaneBeforeBuild, TheBuildRestartsABoundedNumberOfTimes) {
  // A page whose images never cache (a decode that keeps failing) is drawn again and again: the build gives the buffer
  // back only kMaxYields times, then carries on to its end.
  uint8_t yields = 0;
  int restarts = 0;
  for (int render = 0; render < 100; ++render) {
    if (decodeWhileBuilding(yields) == Decode::TakeFromBuild) {
      ++yields;
      ++restarts;
    }
  }
  EXPECT_EQ(kMaxYields, restarts);
}

}  // namespace

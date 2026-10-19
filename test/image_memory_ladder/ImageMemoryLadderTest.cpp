// ImageMemoryLadder: what the reader does next when a page's images do not decode for lack of memory.
#include <gtest/gtest.h>

#include "activities/reader/ImageMemoryLadder.h"

namespace {

using ImageMemoryLadder::Action;
using ImageMemoryLadder::Decode;
using ImageMemoryLadder::Input;
using ImageMemoryLadder::next;
using ImageMemoryLadder::Step;

// A TrueType reader font and three accent families, nothing in the way of unloading them: the four-family case.
Input fourFamilies(const Step step, const Decode decode) {
  Input in;
  in.step = step;
  in.decode = decode;
  in.ttfLoaded = true;
  in.canUnload = true;
  in.accents = 3;
  return in;
}

TEST(ImageMemoryLadder, ADecodedPageIsDrawnAtWhicheverStepItDecoded) {
  for (const Step step : {Step::Plain, Step::LightRelief, Step::HeavyRelief}) {
    EXPECT_EQ(Action::Done, next(fourFamilies(step, Decode::Done))) << static_cast<int>(step);
  }
}

TEST(ImageMemoryLadder, ADecodeShortOfMemoryClimbsOneStepAtATime) {
  EXPECT_EQ(Action::LightRelief, next(fourFamilies(Step::Plain, Decode::HeapShort)));
  EXPECT_EQ(Action::HeavyRelief, next(fourFamilies(Step::LightRelief, Decode::HeapShort)));
  // Every TrueType family was unloaded for that decode: fewer families cannot make room for it.
  EXPECT_EQ(Action::Emergency, next(fourFamilies(Step::HeavyRelief, Decode::HeapShort)));
}

TEST(ImageMemoryLadder, ACoarsePictureWithNoRoomForAFinerOneDoesNotClimb) {
  Input in = fourFamilies(Step::Plain, Decode::Coarse);
  in.coarseFits = false;
  EXPECT_EQ(Action::Done, next(in)) << "drawn as it is: no relief, no decode for nothing";
}

TEST(ImageMemoryLadder, ACoarsePictureClimbsToTheLightStepAndNoFurther) {
  Input in = fourFamilies(Step::Plain, Decode::Coarse);
  in.coarseFits = true;
  EXPECT_EQ(Action::LightRelief, next(in));
  // Coarse again (or the room was not there after all): the coarse picture is drawn, never a box. Never the heavy
  // step either, which unloads the TrueType set for a picture that is drawn either way.
  in.step = Step::LightRelief;
  EXPECT_EQ(Action::Done, next(in));
  in.step = Step::HeavyRelief;
  EXPECT_EQ(Action::Done, next(in));
}

TEST(ImageMemoryLadder, ADecodeThatFailsForAnotherReasonIsLeftAlone) {
  for (const Step step : {Step::Plain, Step::LightRelief, Step::HeavyRelief}) {
    EXPECT_EQ(Action::Done, next(fourFamilies(step, Decode::Failed))) << static_cast<int>(step);
  }
}

TEST(ImageMemoryLadder, WithoutATrueTypeSetTheLightStepIsTheLast) {
  Input in = fourFamilies(Step::Plain, Decode::HeapShort);
  in.ttfLoaded = false;
  in.accents = 0;
  EXPECT_EQ(Action::LightRelief, next(in));  // the built-in font caches still go
  in.step = Step::LightRelief;
  EXPECT_EQ(Action::Emergency, next(in));
}

TEST(ImageMemoryLadder, ALiveSectionBuildKeepsTheSetLoaded) {
  Input in = fourFamilies(Step::LightRelief, Decode::HeapShort);
  in.canUnload = false;
  EXPECT_EQ(Action::Emergency, next(in));
}

TEST(ImageMemoryLadder, ASetThatDoesNotComeBackLosesItsLeastUsedAccentFamily) {
  for (const Step step : {Step::LightRelief, Step::HeavyRelief}) {
    for (const Decode decode : {Decode::Done, Decode::Coarse, Decode::HeapShort, Decode::Failed}) {
      Input in = fourFamilies(step, decode);
      in.fontsBack = false;
      EXPECT_EQ(Action::DropAccent, next(in)) << static_cast<int>(step) << " " << static_cast<int>(decode);
      in.accents = 1;
      EXPECT_EQ(Action::DropAccent, next(in));
    }
  }
}

TEST(ImageMemoryLadder, WhenOnlyTheBodyIsLeftAndItDoesNotComeBackItIsARestart) {
  for (const Step step : {Step::LightRelief, Step::HeavyRelief}) {
    Input in = fourFamilies(step, Decode::Done);
    in.accents = 0;
    in.fontsBack = false;
    // A set without its miss memory would lay text out with glyphs it cannot draw: never left so.
    EXPECT_EQ(Action::Restart, next(in)) << static_cast<int>(step);
  }
}

}  // namespace

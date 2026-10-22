#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "AutoScrollController.h"
#include "SongbookTestSupport.h"

using songbook::AutoScrollController;
using State = songbook::AutoScrollController::State;

namespace {

// Stops 100 px apart, a 600 px song in a 300 px view, one minute long.
AutoScrollController uniform() {
  AutoScrollController controller;
  controller.configure({0, 100, 200, 300}, 600, 300, 60000);
  return controller;
}

}  // namespace

TEST(AutoScrollController, ScheduleFollowsReadingPace) {
  const auto controller = uniform();
  // Due when the reader reaches 1/3 of the view below the stop: D * (stop + 100) / 600.
  EXPECT_EQ(controller.stepTimeMs(0), 0u);
  EXPECT_EQ(controller.stepTimeMs(1), 20000u);
  EXPECT_EQ(controller.stepTimeMs(2), 30000u);
  EXPECT_EQ(controller.stepTimeMs(3), 40000u);
  EXPECT_EQ(controller.state(), State::Idle);
}

TEST(AutoScrollController, AdvancesOnTimer) {
  auto controller = uniform();
  controller.start(1000);
  EXPECT_FALSE(controller.update(1000 + 19999));
  EXPECT_EQ(controller.step(), 0u);
  EXPECT_TRUE(controller.update(1000 + 20000));
  EXPECT_EQ(controller.step(), 1u);
  EXPECT_FALSE(controller.update(1000 + 25000));
}

TEST(AutoScrollController, LateUpdatesDoNotDrift) {
  auto controller = uniform();
  controller.start(0);
  // A slow refresh delays polling; the next poll catches up from elapsed time.
  EXPECT_TRUE(controller.update(31500));
  EXPECT_EQ(controller.step(), 2u);
  EXPECT_TRUE(controller.update(40000));
  EXPECT_EQ(controller.step(), 3u);
}

TEST(AutoScrollController, PauseFreezesAndResumeContinues) {
  auto controller = uniform();
  controller.start(0);
  controller.pause(10000);
  EXPECT_EQ(controller.state(), State::Paused);
  EXPECT_TRUE(controller.active());
  EXPECT_FALSE(controller.update(500000));
  EXPECT_EQ(controller.elapsedMs(500000), 10000u);
  controller.resume(500000);
  EXPECT_FALSE(controller.update(500000 + 9999));
  EXPECT_TRUE(controller.update(500000 + 10000));
  EXPECT_EQ(controller.step(), 1u);
}

TEST(AutoScrollController, StopsAtEndWithoutAdvancingFurther) {
  auto controller = uniform();
  controller.start(0);
  EXPECT_TRUE(controller.update(60000));
  EXPECT_EQ(controller.state(), State::Finished);
  EXPECT_EQ(controller.step(), 3u);
  EXPECT_FALSE(controller.active());
  EXPECT_FALSE(controller.update(10'000'000));
  EXPECT_EQ(controller.step(), 3u);
}

TEST(AutoScrollController, ManualStepShiftsTimeline) {
  auto controller = uniform();
  controller.start(0);
  EXPECT_TRUE(controller.stepBy(+1, 5000));
  EXPECT_EQ(controller.step(), 1u);
  EXPECT_EQ(controller.elapsedMs(5000), 20000u);
  // The next step keeps its spacing from the manual one.
  EXPECT_FALSE(controller.update(5000 + 9999));
  EXPECT_TRUE(controller.update(5000 + 10000));
  EXPECT_EQ(controller.step(), 2u);
  EXPECT_TRUE(controller.stepBy(-2, 20000));
  EXPECT_EQ(controller.step(), 0u);
  EXPECT_EQ(controller.elapsedMs(20000), 0u);
  EXPECT_FALSE(controller.stepBy(-1, 20000));
}

TEST(AutoScrollController, StartsFromBrowsedStop) {
  auto controller = uniform();
  EXPECT_TRUE(controller.stepBy(+2, 0));
  controller.start(100);
  EXPECT_EQ(controller.elapsedMs(100), 30000u);
  EXPECT_FALSE(controller.update(100));
  EXPECT_EQ(controller.step(), 2u);
}

TEST(AutoScrollController, StopResetsToTop) {
  auto controller = uniform();
  controller.start(0);
  controller.update(35000);
  controller.stop();
  EXPECT_EQ(controller.state(), State::Idle);
  EXPECT_EQ(controller.step(), 0u);
  EXPECT_EQ(controller.elapsedMs(99999), 0u);
}

TEST(AutoScrollController, FinishedRestartsFromTop) {
  auto controller = uniform();
  controller.start(0);
  controller.update(60000);
  controller.start(70000);
  EXPECT_EQ(controller.state(), State::Playing);
  EXPECT_EQ(controller.step(), 0u);
}

TEST(AutoScrollController, SongThatFitsHasOneStop) {
  AutoScrollController controller;
  controller.configure({0}, 200, 300, 30000);
  EXPECT_EQ(controller.stepCount(), 1u);
  controller.start(0);
  EXPECT_FALSE(controller.update(29999));
  EXPECT_TRUE(controller.update(30000));
  EXPECT_EQ(controller.state(), State::Finished);
  EXPECT_EQ(controller.step(), 0u);
}

TEST(AutoScrollController, SurvivesMillisWrap) {
  auto controller = uniform();
  const uint32_t start = UINT32_MAX - 5000;
  controller.start(start);
  EXPECT_TRUE(controller.update(start + 20000u));  // wraps past zero
  EXPECT_EQ(controller.step(), 1u);
}

TEST(AutoScrollController, TimestampBeforeStartIsNotAFinish) {
  // A loop pass may read the clock before a song load and start the timer after it.
  auto controller = uniform();
  controller.start(5000);
  EXPECT_FALSE(controller.update(4800));
  EXPECT_EQ(controller.state(), State::Playing);
  EXPECT_EQ(controller.elapsedMs(4800), 0u);
  EXPECT_TRUE(controller.update(5000 + 20000));
  EXPECT_EQ(controller.step(), 1u);
}

TEST(AutoScrollController, VeryLongSongScheduleIsMonotonic) {
  songbook::Song song = songbook_test::parse([] {
    std::string source = "{sov}\n";
    for (int i = 0; i < 1500; ++i) source += "[Am]Line " + std::to_string(i) + "\n";
    return source + "{eov}\n";
  }());
  songbook::SongLayout layout;
  layout.buildLyrics(song, songbook_test::FixedMeasurer{}, songbook::Viewport{480, 650}, songbook::SectionNames{});
  AutoScrollController controller;
  controller.configure(layout.scrollStops(), layout.contentHeight(), 650, 600000);
  ASSERT_GT(controller.stepCount(), 100u);
  for (size_t k = 1; k < controller.stepCount(); ++k) {
    EXPECT_GT(controller.stepTimeMs(k), controller.stepTimeMs(k - 1));
  }
  EXPECT_LT(controller.stepTimeMs(controller.stepCount() - 1), 600000u);
}

TEST(AutoScrollController, ZeroHeightIsSafe) {
  AutoScrollController controller;
  controller.configure({0, 10}, 0, 300, 1000);
  EXPECT_EQ(controller.stepCount(), 1u);
  controller.start(0);
  EXPECT_TRUE(controller.update(1000));
}

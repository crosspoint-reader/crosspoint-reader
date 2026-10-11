// Cover for where each home screen entry ends up: on the home screen, behind its "More" entry,
// or nowhere. The one promise the "More" entry makes is that hiding an entry never makes it
// unreachable -- and, the other way round, that a More row never opens onto an empty list.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/home/HomeMenu.h"
#include "activities/home/LibraryTab.h"

CrossPointSettings CrossPointSettings::instance;

namespace {

using A = HomeMenuAction;

std::vector<A> actionsAt(const HomeMenuPlacement placement, const HomeMenuAvailability& availability) {
  std::vector<HomeMenuEntry> entries;
  collectHomeMenuEntries(placement, availability, entries);
  std::vector<A> actions;
  for (const HomeMenuEntry& entry : entries) actions.push_back(entry.action);
  return actions;
}

class HomeMenuTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SETTINGS.showLibraryOnHome = 1;
    SETTINGS.showReadingStatsOnHome = 1;
    SETTINGS.showBookmarksOnHome = 1;
    SETTINGS.showFileTransferOnHome = 1;
    SETTINGS.showWeatherOnHome = 1;
    SETTINGS.useWeather = 1;
  }

  HomeMenuAvailability everything{.hasBookmarks = true};
  HomeMenuAvailability nothingOptional{.hasBookmarks = false};
};

TEST_F(HomeMenuTest, DefaultsShowEverythingAvailableAndNoMore) {
  EXPECT_EQ(
      actionsAt(HomeMenuPlacement::Home, everything),
      (std::vector<A>{A::Library, A::ReadingStats, A::GlobalBookmarks, A::FileTransfer, A::Weather, A::Settings}));
  EXPECT_TRUE(actionsAt(HomeMenuPlacement::More, everything).empty());
}

TEST_F(HomeMenuTest, EntriesWithNothingBehindThemAppearNowhere) {
  EXPECT_EQ(actionsAt(HomeMenuPlacement::Home, nothingOptional),
            (std::vector<A>{A::Library, A::ReadingStats, A::FileTransfer, A::Weather, A::Settings}));
}

TEST_F(HomeMenuTest, AHiddenEntryMovesBehindMoreWhichSitsAboveSettings) {
  SETTINGS.showFileTransferOnHome = 0;

  EXPECT_EQ(actionsAt(HomeMenuPlacement::Home, everything),
            (std::vector<A>{A::Library, A::ReadingStats, A::GlobalBookmarks, A::Weather, A::More, A::Settings}));
  EXPECT_EQ(actionsAt(HomeMenuPlacement::More, everything), (std::vector<A>{A::FileTransfer}));
}

TEST_F(HomeMenuTest, MoreKeepsTheHomeScreenOrder) {
  SETTINGS.showWeatherOnHome = 0;
  SETTINGS.showLibraryOnHome = 0;
  SETTINGS.showFileTransferOnHome = 0;

  EXPECT_EQ(actionsAt(HomeMenuPlacement::More, everything), (std::vector<A>{A::Library, A::FileTransfer, A::Weather}));
}

// Hiding Bookmarks with no bookmark saved hides nothing the user could open, so a More row here
// would lead to an empty list.
TEST_F(HomeMenuTest, HidingAnUnavailableEntryAddsNoMore) {
  SETTINGS.showBookmarksOnHome = 0;

  const auto home = actionsAt(HomeMenuPlacement::Home, nothingOptional);
  EXPECT_EQ(std::count(home.begin(), home.end(), A::More), 0);
  EXPECT_TRUE(actionsAt(HomeMenuPlacement::More, nothingOptional).empty());
}

TEST_F(HomeMenuTest, WeatherTurnedOffIsInNeitherPlace) {
  SETTINGS.useWeather = 0;
  SETTINGS.showWeatherOnHome = 0;

  const auto home = actionsAt(HomeMenuPlacement::Home, everything);
  EXPECT_EQ(std::count(home.begin(), home.end(), A::Weather), 0);
  EXPECT_EQ(std::count(home.begin(), home.end(), A::More), 0);
  EXPECT_TRUE(actionsAt(HomeMenuPlacement::More, everything).empty());
}

TEST_F(HomeMenuTest, SettingsStaysWhenEverythingElseIsHidden) {
  SETTINGS.showLibraryOnHome = 0;
  SETTINGS.showReadingStatsOnHome = 0;
  SETTINGS.showBookmarksOnHome = 0;
  SETTINGS.showFileTransferOnHome = 0;
  SETTINGS.showWeatherOnHome = 0;

  EXPECT_EQ(actionsAt(HomeMenuPlacement::Home, everything), (std::vector<A>{A::More, A::Settings}));
  EXPECT_EQ(actionsAt(HomeMenuPlacement::More, everything),
            (std::vector<A>{A::Library, A::ReadingStats, A::GlobalBookmarks, A::FileTransfer, A::Weather}));
}

// Every entry is reachable from exactly one place, whatever the settings say.
TEST_F(HomeMenuTest, NoEntryIsInBothPlaces) {
  SETTINGS.showLibraryOnHome = 0;
  SETTINGS.showWeatherOnHome = 0;

  const auto home = actionsAt(HomeMenuPlacement::Home, everything);
  for (const A action : actionsAt(HomeMenuPlacement::More, everything)) {
    EXPECT_EQ(std::count(home.begin(), home.end(), action), 0) << static_cast<int>(action);
  }
}

// Browse Files and Recent Books became the Library's tabs: one entry, under its own name.
TEST_F(HomeMenuTest, OneLibraryEntryLabelledLibrary) {
  std::vector<HomeMenuEntry> entries;
  collectHomeMenuEntries(HomeMenuPlacement::Home, everything, entries);
  ASSERT_FALSE(entries.empty());
  EXPECT_EQ(entries.front().action, A::Library);
  EXPECT_EQ(entries.front().label, StrId::STR_LIBRARY);
  EXPECT_EQ(entries.front().icon, Folder);
}

// Browse Files and Recent Books had a home entry each; the Library is shown where either was.
TEST(HomeMenuMigration, LibraryShownWhereEitherOldEntryWas) {
  EXPECT_EQ(libraryOnHomeFromLegacy(1, 1), 1);
  EXPECT_EQ(libraryOnHomeFromLegacy(1, 0), 1);
  EXPECT_EQ(libraryOnHomeFromLegacy(0, 1), 1);
  EXPECT_EQ(libraryOnHomeFromLegacy(0, 0), 0);
}

TEST(LibraryTabs, StoredValueOutOfRangeOpensBooks) {
  EXPECT_EQ(libraryTabFrom(2), LibraryTab::New);
  EXPECT_EQ(libraryTabFrom(LIBRARY_TAB_COUNT), LibraryTab::Books);
  EXPECT_EQ(libraryTabFrom(255), LibraryTab::Books);
}

TEST(LibraryTabs, StepWrapsAtBothEnds) {
  EXPECT_EQ(libraryTabStep(LibraryTab::Books, 1), LibraryTab::Recent);
  EXPECT_EQ(libraryTabStep(LibraryTab::Authors, 1), LibraryTab::Books);
  EXPECT_EQ(libraryTabStep(LibraryTab::Books, -1), LibraryTab::Authors);
}

}  // namespace

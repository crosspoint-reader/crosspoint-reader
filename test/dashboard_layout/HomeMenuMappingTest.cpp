#include <gtest/gtest.h>

#include "activities/home/HomeMenuMapping.h"

TEST(HomeMenuMapping, OptionalEntriesDoNotMoveExistingItemsUnexpectedly) {
  for (const bool hasReadingStats : {false, true}) {
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::FILE_BROWSER, false, hasReadingStats), 0);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::RECENTS, false, hasReadingStats), 1);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::SAVED_ITEMS, false, hasReadingStats), 2);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::FILE_TRANSFER, false, hasReadingStats), 3);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::SETTINGS_MENU, false, hasReadingStats), 4);

    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::FILE_BROWSER, true, hasReadingStats), 0);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::RECENTS, true, hasReadingStats), 1);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::SAVED_ITEMS, true, hasReadingStats), 2);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::OPDS_BROWSER, true, hasReadingStats), 3);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::FILE_TRANSFER, true, hasReadingStats), 4);
    EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::SETTINGS_MENU, true, hasReadingStats), 5);
  }
}

TEST(HomeMenuMapping, ReadingStatsIsPresentOnlyWhenAvailableAndAlwaysAppended) {
  EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, false, false), -1);
  EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, true, false), -1);
  EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, false, true), 5);
  EXPECT_EQ(HomeMenuMapping::indexOf(HomeMenuItem::READING_STATS, true, true), 6);
  EXPECT_EQ(HomeMenuMapping::itemCount(false, true), 6);
  EXPECT_EQ(HomeMenuMapping::itemCount(true, true), 7);
}

TEST(HomeMenuMapping, IndexRoundTripsAcrossOptionalStates) {
  for (const int recentBookCount : {0, 1, 3}) {
    for (const bool hasOpds : {false, true}) {
      for (const bool hasReadingStats : {false, true}) {
        const int menuCount = HomeMenuMapping::itemCount(hasOpds, hasReadingStats);
        EXPECT_EQ(HomeMenuMapping::selectionCount(recentBookCount, hasOpds, hasReadingStats),
                  recentBookCount + menuCount);
        for (int menuIndex = 0; menuIndex < menuCount; ++menuIndex) {
          const HomeMenuItem action = HomeMenuMapping::actionAt(menuIndex, hasOpds, hasReadingStats);
          EXPECT_NE(action, HomeMenuItem::NONE);
          EXPECT_EQ(HomeMenuMapping::indexOf(action, hasOpds, hasReadingStats), menuIndex);
          EXPECT_EQ(HomeMenuMapping::selectorIndexOf(action, recentBookCount, hasOpds, hasReadingStats),
                    recentBookCount + menuIndex);
        }
        EXPECT_EQ(HomeMenuMapping::actionAt(-1, hasOpds, hasReadingStats), HomeMenuItem::NONE);
        EXPECT_EQ(HomeMenuMapping::actionAt(menuCount, hasOpds, hasReadingStats), HomeMenuItem::NONE);
      }
    }
  }
}

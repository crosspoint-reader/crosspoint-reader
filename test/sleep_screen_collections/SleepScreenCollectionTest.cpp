#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "HalStorage.h"
#include "util/SleepScreenCollection.h"

class SleepScreenCollectionTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(SleepScreenCollectionTest, RootImagesRemainTheDefaultSet) {
  Storage.addFile("/sleep/a.bmp");
  Storage.addFile("/sleep/b.bmp");

  std::vector<std::string> sets;
  SleepScreenCollection::discover("/sleep", sets);
  EXPECT_EQ(sets, std::vector<std::string>({""}));
}

TEST_F(SleepScreenCollectionTest, DiscoversDefaultAndFirstLevelSetsInNaturalOrder) {
  Storage.addFile("/.sleep/root.bmp");
  Storage.addFile("/.sleep/Photos/photo.bmp");
  Storage.addFile("/.sleep/Art/art.bmp");

  std::vector<std::string> sets;
  SleepScreenCollection::discover("/.sleep", sets);
  EXPECT_EQ(sets, std::vector<std::string>({"", "Art", "Photos"}));
}

TEST_F(SleepScreenCollectionTest, BoundsNamedSetsAndKeepsDefault) {
  Storage.addFile("/sleep/root.bmp");
  for (size_t i = 0; i < SleepScreenCollection::MAX_NAMED_SET_COUNT + 5; i++) {
    Storage.addFile("/sleep/Set" + std::to_string(i) + "/image.bmp");
  }

  std::vector<std::string> sets;
  SleepScreenCollection::discover("/sleep", sets);
  ASSERT_EQ(sets.size(), SleepScreenCollection::MAX_NAMED_SET_COUNT + 1);
  EXPECT_TRUE(sets.front().empty());
}

TEST_F(SleepScreenCollectionTest, NestedFoldersAndTheirImagesAreIgnored) {
  Storage.addFile("/sleep/Photos/a.bmp");
  Storage.addFile("/sleep/Photos/2025/b.bmp");
  Storage.addFile("/sleep/Photos/Trips/Ireland/c.bmp");
  Storage.addFile("/sleep/NestedOnly/Trips/ireland.bmp");

  std::vector<std::string> sets;
  SleepScreenCollection::discover("/sleep", sets);
  EXPECT_EQ(sets, std::vector<std::string>({"Photos"}));

  std::string selectedDirectory;
  EXPECT_TRUE(SleepScreenCollection::resolveSelectedDirectory("/sleep", "Photos", selectedDirectory));
  EXPECT_EQ(selectedDirectory, "/sleep/Photos");
}

TEST_F(SleepScreenCollectionTest, IgnoresEmptyInvalidAndHiddenFolders) {
  Storage.addFile("/sleep/Empty/notes.txt");
  Storage.addDirectory("/sleep/ActuallyEmpty");
  Storage.addFile("/sleep/Invalid/not-really.bmp", false);
  Storage.addFile("/sleep/.hidden/secret.bmp");
  Storage.addFile("/sleep/Valid/a.BMP");

  std::vector<std::string> sets;
  SleepScreenCollection::discover("/sleep", sets);
  EXPECT_EQ(sets, std::vector<std::string>({"Valid"}));
}

TEST_F(SleepScreenCollectionTest, MissingSelectionResolvesToDefaultDirectory) {
  Storage.addFile("/sleep/root.bmp");
  Storage.addFile("/sleep/Art/art.bmp");

  std::string selectedDirectory;
  EXPECT_FALSE(SleepScreenCollection::resolveSelectedDirectory("/sleep", "Missing", selectedDirectory));
  EXPECT_EQ(selectedDirectory, "/sleep");
}

TEST_F(SleepScreenCollectionTest, SkipsSetNamesThatCannotBePersistedLosslessly) {
  const std::string longestSupported(SleepScreenCollection::MAX_SET_NAME_BYTES, 'a');
  const std::string overlong(SleepScreenCollection::MAX_SET_NAME_BYTES + 1, 'b');
  Storage.addFile("/sleep/" + longestSupported + "/valid.bmp");
  Storage.addFile("/sleep/" + overlong + "/ignored.bmp");

  std::vector<std::string> sets;
  SleepScreenCollection::discover("/sleep", sets);
  ASSERT_EQ(sets.size(), 1U);
  EXPECT_EQ(sets.front(), longestSupported);
}

TEST_F(SleepScreenCollectionTest, UnsafePersistedSelectionFallsBackToDefault) {
  Storage.addFile("/sleep/root.bmp");
  Storage.addFile("/sleep/System Volume Information/ignored.bmp");

  std::string selectedDirectory;
  EXPECT_FALSE(
      SleepScreenCollection::resolveSelectedDirectory("/sleep", "System Volume Information", selectedDirectory));
  EXPECT_EQ(selectedDirectory, "/sleep");
}

TEST_F(SleepScreenCollectionTest, PreservesHiddenThenVisibleLegacyDirectoryResolution) {
  Storage.addDirectory("/sleep");
  EXPECT_STREQ(SleepScreenCollection::resolveDirectory(), "/sleep");

  Storage.addDirectory("/.sleep");
  EXPECT_STREQ(SleepScreenCollection::resolveDirectory(), "/.sleep");

  // The legacy root file is intentionally outside directory resolution;
  // SleepActivity continues to render it before calling this helper.
  Storage.addFile("/sleep.bmp");
  EXPECT_STREQ(SleepScreenCollection::resolveDirectory(), "/.sleep");
}

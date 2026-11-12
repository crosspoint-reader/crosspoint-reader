#include <gtest/gtest.h>

#include "HalStorage.h"
#include "NextBookFinder.h"

namespace {
class NextBookFinderTest : public ::testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
};
}  // namespace

TEST_F(NextBookFinderTest, SkipsGeneratedFilesAtRootBeforeLimitingSuggestions) {
  for (const auto* path : {"/A.txt", "/My Clippings.txt", "/CRASH_REPORT.TXT", "/Z.txt", "/zz.epub"}) {
    fake::add(path);
  }
  const std::vector<std::string> expected = {"Z.txt", "zz.epub"};
  EXPECT_EQ(NextBookFinder::findNextBooks("/A.txt", 2), expected);
}

TEST_F(NextBookFinderTest, KeepsSameNamedFilesInSubfolders) {
  for (const auto* path : {"/Books/A.txt", "/Books/My Clippings.txt", "/Books/crash_report.txt"}) {
    fake::add(path);
  }
  const std::vector<std::string> expected = {"crash_report.txt", "My Clippings.txt"};
  EXPECT_EQ(NextBookFinder::findNextBooks("/Books/A.txt", 5), expected);
}

TEST_F(NextBookFinderTest, KeepsDirectoriesNamedLikeGeneratedFiles) {
  for (const auto* directory : {"/My Clippings.txt", "/crash_report.txt"}) {
    const std::string folder(directory);
    fake::add(folder + "/A.txt");
    fake::add(folder + "/B.txt");
    EXPECT_EQ(NextBookFinder::findNextBooks(folder + "/A.txt", 5), std::vector<std::string>{"B.txt"});
  }
}

TEST_F(NextBookFinderTest, AppliesLibraryResourceDirectoryExclusions) {
  fake::add("/fonts/A.txt");
  fake::add("/fonts/B.txt");
  EXPECT_TRUE(NextBookFinder::findNextBooks("/fonts/A.txt", 5).empty());
}

TEST_F(NextBookFinderTest, PreservesNaturalOrderAndLimit) {
  for (const auto* path : {"/A1.txt", "/A10.txt", "/A3.txt", "/A2.txt", "/A2.bmp"}) {
    fake::add(path);
  }
  const std::vector<std::string> expected = {"A2.txt", "A3.txt"};
  EXPECT_EQ(NextBookFinder::findNextBooks("/A1.txt", 2), expected);
  EXPECT_TRUE(NextBookFinder::findNextBooks("/A1.txt", 0).empty());
  EXPECT_TRUE(NextBookFinder::findNextBooks("", 2).empty());
}

TEST_F(NextBookFinderTest, LongFoldersPreserveFilenameCapacity) {
  const std::string folder = "/" + std::string(200, 'a') + "/" + std::string(200, 'b');
  const std::string nextName = std::string(200, 'z') + ".txt";
  fake::add(folder + "/A.txt");
  fake::add(folder + "/" + nextName);
  EXPECT_EQ(NextBookFinder::findNextBooks(folder + "/A.txt", 2), std::vector<std::string>{nextName});
}

TEST_F(NextBookFinderTest, ReturnsNoSuggestionsWhenPathBufferAllocationFails) {
  fake::add("/A.txt");
  fake::add("/B.txt");
  fake::failAlloc = 0;
  EXPECT_TRUE(NextBookFinder::findNextBooks("/A.txt", 2).empty());
  EXPECT_TRUE(fake::failureTriggered);
}

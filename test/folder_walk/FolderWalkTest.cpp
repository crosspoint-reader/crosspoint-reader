// FolderWalk::eachFile(): every file below a folder, once, from a walk that closes each folder on
// the way down and seeks back into its listing on the way up. Runs over a real directory tree
// through the stdio storage shim, whose folder positions stand in for SdFat's.
#include <FolderWalk.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class FolderWalkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ = (fs::path(FOLDER_WALK_ROOT) / info->name()).generic_string();
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  void TearDown() override { fs::remove_all(root_); }

  // Makes the file `rel` below the root and returns the path the walk should hand over.
  std::string file(const std::string& rel) {
    const fs::path path = fs::path(root_) / rel;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "x";
    return path.generic_string();
  }
  void folder(const std::string& rel) const { fs::create_directories(fs::path(root_) / rel); }

  // Every path handed over, with how often.
  std::map<std::string, int> walk(const std::string& dir, bool* ok = nullptr) {
    std::map<std::string, int> seen;
    const bool walked = FolderWalk::eachFile(
        dir, [](const std::string& path, void* ctx) { ++(*static_cast<std::map<std::string, int>*>(ctx))[path]; },
        &seen);
    if (ok != nullptr) *ok = walked;
    return seen;
  }
  static std::map<std::string, int> once(const std::vector<std::string>& paths) {
    std::map<std::string, int> expected;
    for (const auto& path : paths) expected[path] = 1;
    return expected;
  }

  std::string root_;
};

// The files listed after a folder are the ones the walk reaches by seeking back into the listing it
// left for that folder.
TEST_F(FolderWalkTest, FilesAroundFoldersAreEachHandedOverOnce) {
  const std::vector<std::string> expected = {
      file("a.epub"),       file("b/1.epub"), file("b/c/2.epub"), file("b/c/3.txt"),        file("b/d.epub"),
      file("b/e/f/4.epub"), file("b/z.epub"), file("m.epub"),     file("n/only/deep.epub"), file("z.epub"),
  };
  folder("b/empty");
  folder("y/empty/too");

  bool ok = false;
  EXPECT_EQ(walk(root_, &ok), once(expected));
  EXPECT_TRUE(ok);
}

// What the queue of folders to visit paid for with a path each: a level of many folders. Kept
// at 500 because the shim lists a folder afresh on every reopen.
TEST_F(FolderWalkTest, AWideLevelIsWalkedToTheLastFolder) {
  std::vector<std::string> expected;
  for (int i = 0; i < 500; ++i) {
    char rel[32];
    snprintf(rel, sizeof(rel), "Author %04d/book.epub", i);
    expected.push_back(file(rel));
  }
  expected.push_back(file("zz.epub"));

  EXPECT_EQ(walk(root_), once(expected));
}

// Deeper than the levels the walk reserves for, so its stack of positions grows.
TEST_F(FolderWalkTest, ADeepTreeIsWalkedToTheBottom) {
  std::string rel;
  std::vector<std::string> expected;
  for (int level = 0; level < 40; ++level) {
    rel += "d/";
    expected.push_back(file(rel + "book.epub"));
    expected.push_back(file(rel + "x/after.epub"));  // listed after "d": reached on the way back up
  }

  EXPECT_EQ(walk(root_), once(expected));
}

TEST_F(FolderWalkTest, AFolderGivenWithATrailingSlashGivesPlainPaths) {
  const std::string book = file("sub/book.epub");

  EXPECT_EQ(walk(root_ + "/"), once({book}));
}

TEST_F(FolderWalkTest, AnythingButAFolderIsNotWalked) {
  const std::string book = file("book.epub");
  bool ok = true;

  EXPECT_TRUE(walk(book, &ok).empty());
  EXPECT_FALSE(ok);
  EXPECT_TRUE(walk(root_ + "/missing", &ok).empty());
  EXPECT_FALSE(ok);
}

TEST_F(FolderWalkTest, AnEmptyFolderHandsOverNothing) {
  bool ok = false;

  EXPECT_TRUE(walk(root_, &ok).empty());
  EXPECT_TRUE(ok);
}

}  // namespace

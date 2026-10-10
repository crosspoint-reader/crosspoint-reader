#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "Epub.h"
#include "LibraryBuilder.h"
#include "LibraryIndexFile.h"
#include "LibraryText.h"

using namespace library;

namespace {

constexpr char INDEX[] = "/.crosspoint/library.idx";
constexpr char STAGE[] = "/.crosspoint/library.stage";
constexpr char GROUP_STAGE[] = "/.crosspoint/library.stage.g";
// sizeof(GroupSortKey) in LibraryBuilder.cpp
constexpr size_t GROUP_SORT_KEY_BYTES = 16;

std::string numbered(const char* prefix, const unsigned value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%s%04u", prefix, value);
  return text;
}

std::string pathAt(LibraryIndexFile& index, const SortOrder order, const uint16_t row) {
  const uint16_t ordinal = index.ordinalForRow(order, row);
  if (ordinal == 0xFFFF) return {};
  ClixRecord record{};
  if (!index.readRecord(ordinal, record)) return {};
  std::string path;
  return index.readPath(record, path) ? path : std::string();
}

class LibraryBuilderTest : public ::testing::Test {
 protected:
  BuildStats stats;

  void SetUp() override {
    fake::reset();
    bookMetadata.clear();
    fake::add("/a.epub");
    fake::add("/b.epub");
  }

  void initial() { ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series)); }
};

}  // namespace

TEST(LibraryBookFileTest, AcceptsBookFilesTheWalkLists) {
  EXPECT_TRUE(isLibraryBookFile("book.epub"));
  EXPECT_TRUE(isLibraryBookFile("/Books/book.epub"));
  EXPECT_TRUE(isLibraryBookFile("/notes.txt"));
  EXPECT_TRUE(isLibraryBookFile("/a/b/c/readme.md"));
  EXPECT_TRUE(isLibraryBookFile("/Comics/issue.xtc"));
  EXPECT_TRUE(isLibraryBookFile("/Comics/issue.xtch"));
  EXPECT_TRUE(isLibraryBookFile("/Books/./book.epub"));
}

TEST(LibraryBookFileTest, RejectsOtherFilesAndHiddenNames) {
  EXPECT_FALSE(isLibraryBookFile(""));
  EXPECT_FALSE(isLibraryBookFile("/Books/book.pdf"));
  EXPECT_FALSE(isLibraryBookFile("/Books/book.epub.part"));
  EXPECT_FALSE(isLibraryBookFile("/.crosspoint/library.dirty"));
  EXPECT_FALSE(isLibraryBookFile("/.crosspoint/plugins/foo/README.md"));
  EXPECT_FALSE(isLibraryBookFile("/.plugins/foo/notes.txt"));
  EXPECT_FALSE(isLibraryBookFile("/Books/.hidden/book.epub"));
  EXPECT_FALSE(isLibraryBookFile("/Books/._book.epub"));
  EXPECT_FALSE(isLibraryBookFile("._book.epub"));
}

TEST(LibraryBookFileTest, RejectsSystemFoldersWithoutHidingOrdinaryTextBooks) {
  EXPECT_FALSE(isLibraryBookFile("/XTCache/fontPos.txt"));
  EXPECT_FALSE(isLibraryBookFile("/Backup/XTCache/epub/book/language.txt"));
  EXPECT_FALSE(isLibraryBookFile("/System Volume Information/notes.txt"));
  EXPECT_TRUE(isLibraryBookFile("/fontPos.txt"));
  EXPECT_TRUE(isLibraryBookFile("/language.txt"));
  EXPECT_TRUE(isLibraryBookFile("/XTCacheArchive/book.txt"));
}

TEST_F(LibraryBuilderTest, SkipsSystemFolderContents) {
  fake::add("/XTCache/fontPos.txt");
  fake::add("/XTCache/epub/book/language.txt");
  fake::add("/System Volume Information/notes.txt");
  fake::add("/fontPos.txt");
  fake::add("/language.txt");
  fake::add("/XTCacheArchive/book.txt");

  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().bookCount, 5u);
  EXPECT_EQ(fake::directoryEntriesByPath["/XTCache/fontPos.txt"], 0u);
  EXPECT_EQ(fake::directoryEntriesByPath["/XTCache/epub"], 0u);
  EXPECT_EQ(fake::directoryEntriesByPath["/System Volume Information/notes.txt"], 0u);
}

TEST(LibraryBookFileTest, RejectsResourceRootsButNotNestedNamesOrSubstrings) {
  for (const auto* root : {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay"}) {
    const std::string path = std::string("/") + root;
    EXPECT_FALSE(isLibraryBookFile(path + "/LICENSE.txt")) << path;
    EXPECT_FALSE(isLibraryBookFile(path + "/nested/book.epub")) << path;
    EXPECT_FALSE(isLibraryBookFile("/./" + path + "/README.md")) << path;
    EXPECT_TRUE(isLibraryBookFile(path + "-books/book.epub")) << path;
    EXPECT_TRUE(isLibraryBookFile(std::string("/books-") + root + "/book.epub")) << path;
    EXPECT_TRUE(isLibraryBookFile(std::string("/books-") + root + "-archive/book.epub")) << path;
    EXPECT_TRUE(isLibraryBookFile("/Books" + path + "/book.txt")) << path;
    EXPECT_TRUE(isLibraryBookFile(path + ".txt")) << path;
  }
  EXPECT_FALSE(isLibraryBookFile("/FoNtS/family/LICENSE.TXT"));
  EXPECT_FALSE(isLibraryBookFile("/DICTIONARIES/nested/book.epub"));
}

TEST(LibraryBookFileTest, ResourceRootsUseAsciiCaseInsensitiveMatching) {
  for (const auto* name : {"FoNtS", "DiCtIoNaRiEs", "PlUgInS", "SlEeP", "SlEeP-OvErLaY"}) {
    const std::string root = std::string("/") + name;
    EXPECT_FALSE(isLibraryBookFile(root + "/book.epub")) << root;
    EXPECT_TRUE(isLibraryBookFile("/books" + root + "/book.epub")) << root;
    EXPECT_TRUE(isLibraryBookFile(root + "-books/book.epub")) << root;
  }
  EXPECT_TRUE(isLibraryBookFile("/f\xC3\xB6nt/book.epub"));
  EXPECT_TRUE(isLibraryBookFile("/font\xC5\xBF/book.epub"));
}

TEST_F(LibraryBuilderTest, ScansNestedSkippedNamesAndRootSubstringNames) {
  std::vector<std::string> expected = {"/a.epub", "/b.epub"};
  expected.reserve(expected.size() + 5 * 4);
  for (const auto* name : {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay"}) {
    expected.push_back(std::string("/books/") + name + "/nested/book.epub");
    expected.push_back(std::string("/") + name + "-books/nested/book.epub");
    expected.push_back(std::string("/books-") + name + "/nested/book.epub");
    expected.push_back(std::string("/books-") + name + "-archive/nested/book.epub");
  }
  for (const auto& path : expected) fake::add(path);

  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.bookCount(), expected.size());
  std::vector<std::string> actual;
  actual.reserve(index.bookCount());
  for (uint16_t row = 0; row < index.bookCount(); row++) {
    actual.push_back(pathAt(index, SortOrder::TitleAsc, row));
  }
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(actual, expected);
  for (const auto& path : expected) {
    EXPECT_GT(fake::directoryEntriesByPath[path], 0u) << path;
  }
}

TEST_F(LibraryBuilderTest, PrunesResourceRootsWithoutScanningTheirContents) {
  std::vector<std::string> expected = {"/a.epub", "/b.epub"};
  expected.reserve(expected.size() + 6 * 3);
  for (const auto* root : {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay", "FoNtS"}) {
    const std::string path = std::string("/") + root;
    fake::add(path + "/LICENSE.txt");
    fake::add(path + "/nested/book.epub");
    expected.push_back(path + "-books/book.epub");
    expected.push_back("/Books" + path + "/book.txt");
    expected.push_back(path + ".txt");
  }
  for (const auto& path : expected) fake::add(path);

  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.bookCount(), expected.size());
  std::vector<std::string> actual;
  actual.reserve(index.bookCount());
  for (uint16_t row = 0; row < index.bookCount(); row++) {
    actual.push_back(pathAt(index, SortOrder::TitleAsc, row));
  }
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(actual, expected);
  for (const auto* root : {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay", "FoNtS"}) {
    const std::string path = std::string("/") + root;
    EXPECT_EQ(fake::directoryEntriesByPath[path + "/LICENSE.txt"], 0u) << path;
    EXPECT_EQ(fake::directoryEntriesByPath[path + "/nested"], 0u) << path;
    EXPECT_EQ(fake::directoryEntriesByPath[path + "/nested/book.epub"], 0u) << path;
  }
}

TEST_F(LibraryBuilderTest, RootAliasesApplyTheSameResourceExclusions) {
  for (const auto* alias : {"/", "//", "///", "/./", "/.//./", ".", "./", ".//."}) {
    SCOPED_TRACE(alias);
    fake::reset();
    EXPECT_TRUE(isLibraryBookFile(std::string(alias) + "/books/sleep/book.epub"));
    EXPECT_TRUE(isLibraryBookFile(std::string(alias) + "/sleep-books/book.epub"));
    EXPECT_FALSE(isLibraryBookFile(std::string(alias) + "/books/.hidden/book.epub"));
    fake::add("/books/sleep/book.epub");
    fake::add("/sleep-books/book.epub");
    for (const auto* name : {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay"}) {
      const std::string root = std::string("/") + name;
      fake::add(root + "/LICENSE.txt");
      fake::add(root + "/nested/book.epub");
      EXPECT_FALSE(isLibraryBookFile(std::string(alias) + "/" + name + "/LICENSE.txt"));
    }

    ASSERT_TRUE(buildLibraryIndex(alias, stats, true, GroupKind::Series));

    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    ASSERT_EQ(index.bookCount(), 2u);
    std::vector<std::string> actual;
    actual.reserve(index.bookCount());
    for (uint16_t row = 0; row < index.bookCount(); row++) {
      actual.push_back(pathAt(index, SortOrder::TitleAsc, row));
    }
    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(actual, (std::vector<std::string>{"/books/sleep/book.epub", "/sleep-books/book.epub"}));
    for (const auto* name : {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay"}) {
      const std::string root = std::string("/") + name;
      EXPECT_EQ(fake::directoryEntriesByPath[root + "/LICENSE.txt"], 0u);
      EXPECT_EQ(fake::directoryEntriesByPath[root + "/nested"], 0u);
    }
  }
}

TEST_F(LibraryBuilderTest, SubRootScansSkipResourceFoldersAndTheirDescendants) {
  for (const auto* name : {"fonts", "dictionaries", "plugins", "sleep", "sleep-overlay", "FoNtS"}) {
    const std::string root = std::string("/") + name;
    for (const auto* alias : {"", "/", "/./"}) {
      for (const auto* suffix : {"", "/nested"}) {
        const std::string scanRoot = std::string(alias) + root + suffix;
        SCOPED_TRACE(scanRoot);
        fake::reset();
        fake::add(root + "/book.epub");
        fake::add(root + "/nested/book.txt");
        EXPECT_FALSE(isLibraryBookFile(scanRoot + "/book.epub"));

        ASSERT_TRUE(buildLibraryIndex(scanRoot.c_str(), stats, true, GroupKind::Series));

        LibraryIndexFile index;
        ASSERT_TRUE(index.open(INDEX));
        EXPECT_EQ(index.bookCount(), 0u);
        EXPECT_EQ(fake::directoryEntriesByPath[root + "/book.epub"], 0u);
        EXPECT_EQ(fake::directoryEntriesByPath[root + "/nested"], 0u);
        EXPECT_EQ(fake::directoryEntriesByPath[root + "/nested/book.txt"], 0u);
      }
    }
  }
}

TEST_F(LibraryBuilderTest, SubRootScansKeepNestedAndSubstringResourceNames) {
  for (const auto* scanRoot : {"/books", "/books/fonts", "/sleep-books", "/books-sleep"}) {
    SCOPED_TRACE(scanRoot);
    fake::reset();
    const std::string bookPath = std::string(scanRoot) + "/sleep/book.epub";
    fake::add(bookPath);
    EXPECT_TRUE(isLibraryBookFile(bookPath));

    ASSERT_TRUE(buildLibraryIndex(scanRoot, stats, true, GroupKind::Series));

    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    ASSERT_EQ(index.bookCount(), 1u);
    EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, 0), bookPath);
    EXPECT_GT(fake::directoryEntriesByPath[bookPath], 0u);
  }
}

TEST_F(LibraryBuilderTest, XtcTitleAndAuthorComeFromTheFile) {
  fake::add("/Comics/one.xtc");
  fake::add("/Comics/two.xtch");
  bookMetadata["/Comics/one.xtc"].title = "Go Problems";
  bookMetadata["/Comics/one.xtc"].author = "Cho Chikun";
  bookMetadata["/Comics/two.xtch"].title = "Sakata no Go";
  bookMetadata["/Comics/two.xtch"].author = "Sakata Eio";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.header().bookCount, 4u);
  bool sawXtc = false;
  bool sawXtch = false;
  for (uint16_t ordinal = 0; ordinal < index.header().bookCount; ordinal++) {
    ClixRecord record{};
    std::string path;
    std::string title;
    std::string author;
    ASSERT_TRUE(index.readRecord(ordinal, record));
    ASSERT_TRUE(index.readPath(record, path));
    if (path != "/Comics/one.xtc" && path != "/Comics/two.xtch") continue;
    ASSERT_TRUE(index.readTitle(record, title));
    ASSERT_TRUE(index.readAuthor(record, author));
    EXPECT_EQ(title, bookMetadata[path].title);
    EXPECT_EQ(author, bookMetadata[path].author);
    (path == "/Comics/one.xtc" ? sawXtc : sawXtch) = true;
  }
  EXPECT_TRUE(sawXtc);
  EXPECT_TRUE(sawXtch);
}

TEST_F(LibraryBuilderTest, UnchangedRebuildReusesMetadataAndDoesNotReplaceIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));

  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.parsed, 0);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, FolderHeavyUnchangedReconciliationIoScalesLinearly) {
  const auto measure = [this](const unsigned count) {
    fake::reset();
    bookMetadata.clear();
    for (unsigned i = 0; i < count; i++) {
      fake::add("/folder" + numbered("", i) + "/book.txt");
    }
    if (!buildLibraryIndex("/", stats, false, GroupKind::Series)) {
      ADD_FAILURE() << "initial build failed for " << count << " books";
      return 0u;
    }
    fake::resetIoCounters();
    if (!buildLibraryIndex("/", stats, false, GroupKind::Series)) {
      ADD_FAILURE() << "unchanged build failed for " << count << " books";
      return 0u;
    }
    EXPECT_EQ(stats.metadataReused, count);
    EXPECT_FALSE(stats.indexReplaced);
    return fake::reads + fake::seeks;
  };

  const unsigned smallIo = measure(128);
  const unsigned largeIo = measure(256);
  EXPECT_LT(largeIo, smallIo * 3u);
}

TEST_F(LibraryBuilderTest, DirectoryEntriesAreEnumeratedOnce) {
  fake::add("/folder/c.txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Series));

  EXPECT_EQ(fake::directoryEntriesByPath["/a.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/b.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder/c.txt"], 1u);
}

TEST_F(LibraryBuilderTest, DirectoryResumeFailureRetainsPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::add("/aa-folder/c.txt");
  fake::failDirectorySeek = true;

  EXPECT_FALSE(buildLibraryIndex("/", stats, false, GroupKind::Series));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, StagingAndIndexWritesAreBatched) {
  fake::reset();
  for (unsigned i = 0; i < 128; i++) fake::add("/book" + numbered("", i) + ".txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Series));

  EXPECT_LT(fake::writesByPath["/.crosspoint/library.stage"], 64u);
  EXPECT_LT(fake::writesByPath["/.crosspoint/library.new"], 32u);
}

TEST_F(LibraryBuilderTest, ParentDuplicateTrackingSurvivesDirectoryRecursion) {
  fake::add("/folder/c.txt");
  fake::duplicateDirectoryEntry("/a.epub");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Series));

  EXPECT_EQ(stats.books, 3);
  EXPECT_EQ(stats.duplicatesDropped, 1);
}

TEST_F(LibraryBuilderTest, TimestampAndSizeChangesParseOnlyTheChangedBook) {
  initial();
  fake::files["/a.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);

  fake::files["/b.epub"]->bytes.push_back('x');
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
}

TEST_F(LibraryBuilderTest, ZeroTimestampAndFailedExtractionAreNeverFresh) {
  fake::files["/a.epub"]->time = 0;
  bookMetadata["/b.epub"].success = false;
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));

  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);
}

TEST_F(LibraryBuilderTest, MetadataModeChangesInvalidateCachedMetadata) {
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Series));
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
  index.close();

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::parses, 2u);
}

TEST_F(LibraryBuilderTest, EmptyLibraryRebuildFollowsTheKindAndMetadataMode) {
  fake::files.erase("/a.epub");
  fake::files.erase("/b.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    EXPECT_EQ(index.bookCount(), 0);
    EXPECT_EQ(index.groupKind(), GroupKind::Series);
  }

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  EXPECT_TRUE(stats.indexReplaced);
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    EXPECT_EQ(index.groupKind(), GroupKind::Publisher);
  }

  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Publisher));
  EXPECT_TRUE(stats.indexReplaced);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
  EXPECT_EQ(index.groupKind(), GroupKind::None);
}

TEST_F(LibraryBuilderTest, RebuildVotesFromSourceAuthorInsteadOfPriorCanonicalAuthor) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].author = "Victor Hugo";
  bookMetadata["/b.epub"].author = "Hugo Victor";
  bookMetadata["/c.epub"].author = "Hugo Victor";
  initial();
  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.remove("/c.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));

  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  std::string author;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "Victor Hugo");
}

TEST_F(LibraryBuilderTest, EqualBasenamesInDifferentFoldersReconcileIndependently) {
  fake::add("/one/same.epub");
  fake::add("/two/same.epub");
  bookMetadata["/one/same.epub"].title = "One";
  bookMetadata["/two/same.epub"].title = "Two";
  initial();
  fake::files["/two/same.epub"]->time++;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));

  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 3);
}

TEST_F(LibraryBuilderTest, ArrivalOrderFollowsModificationTimeOverDiscoveryOrder) {
  // a and b exist with the default time; c lands with an older timestamp and d
  // with the newest, so file times, not walk or firstSeen order, decide.
  fake::add("/c.epub", "book c", /*time=*/0);
  fake::add("/d.epub", "book d", /*time=*/9);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 3), "/d.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentDesc, 0), "/d.epub");
}

TEST_F(LibraryBuilderTest, AddedRemovedMovedAndRenamedBooksKeepArrivalOrder) {
  initial();
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.rename("/a.epub", "/moved.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/moved.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.rename("/moved.epub", "/renamed.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/renamed.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
}

TEST_F(LibraryBuilderTest, WholeFolderRenameWithUniqueSizePreservesArrivalOrder) {
  fake::add("/old/unique.epub", "a uniquely sized book");
  initial();
  ASSERT_TRUE(Storage.mkdir("/new"));
  ASSERT_TRUE(Storage.rename("/old/unique.epub", "/new/unique.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));

  EXPECT_EQ(stats.renamed, 1);
  EXPECT_EQ(stats.removed, 0);
  EXPECT_EQ(fake::parses, 1u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/new/unique.epub");
}

TEST_F(LibraryBuilderTest, DuplicateDetectionRemainsBoundedAndFindsTrackedKeysAfterTheCap) {
  fake::duplicateDirectoryEntry("/a.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(stats.books, 2);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_FALSE(stats.dedupDegraded);

  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i <= LIBRARY_MAX_DEDUP_KEYS; i++) {
    fake::add("/book" + numbered("", i) + ".txt");
  }
  fake::duplicateDirectoryEntry("/book0000.txt");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Series));
  EXPECT_EQ(stats.books, LIBRARY_MAX_DEDUP_KEYS + 1);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_TRUE(stats.dedupDegraded);
  EXPECT_LT(fake::delays, 2000u);
}

TEST_F(LibraryBuilderTest, ReadWriteCloseAndAllocationFailuresRetainPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;

  fake::failRead = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failRead = -1;

  fake::files["/a.epub"]->time++;
  fake::failWrite = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failWrite = -1;

  fake::failWritePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failClosePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failAlloc = 3;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failAlloc = -1;

  fake::failRename = 1;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, TruncatedPersistedPathHashAbortsAndRetainsTheLiveIndex) {
  initial();
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  ClixRecord record{};
  std::memcpy(&record, bytes.data() + recordOffset(header, 0), sizeof(record));
  record.nameOff = header.nameLen - 4;
  std::memcpy(bytes.data() + recordOffset(header, 0), &record, sizeof(record));
  const auto corrupted = bytes;

  EXPECT_FALSE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::files[INDEX]->bytes, corrupted);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
}

TEST_F(LibraryBuilderTest, LibrariesPastOldGateAndAtFormatCeilingKeepAllOrders) {
  for (const unsigned count : {513u, static_cast<unsigned>(CLIX_MAX_RECORDS)}) {
    fake::reset();
    bookMetadata.clear();
    std::vector<unsigned> authorOrder(count);
    std::iota(authorOrder.begin(), authorOrder.end(), 0u);
    for (unsigned i = 0; i < count; i++) {
      const std::string path = "/book" + numbered("", i) + ".epub";
      fake::add(path);
      bookMetadata[path].title = numbered("Title ", count - 1 - i);
      bookMetadata[path].author = numbered("Writer ", (i * (count == 513 ? 257u : 2053u)) % count);
    }

    ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series)) << count;
    ASSERT_EQ(stats.books, count);
    EXPECT_FALSE(stats.ranksDegraded);

    if (count == CLIX_MAX_RECORDS) {
      const auto old = fake::files[INDEX]->bytes;
      fake::parses = 0;
      fake::resetIoCounters();
      ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
      EXPECT_EQ(fake::parses, 0u);
      EXPECT_EQ(stats.metadataReused, CLIX_MAX_RECORDS);
      EXPECT_FALSE(stats.indexReplaced);
      EXPECT_EQ(fake::files[INDEX]->bytes, old);
      EXPECT_LT(fake::delays, 10000u);
    }

    std::sort(authorOrder.begin(), authorOrder.end(), [count](const unsigned a, const unsigned b) {
      return (a * (count == 513 ? 257u : 2053u)) % count < (b * (count == 513 ? 257u : 2053u)) % count;
    });
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    for (uint16_t row = 0; row < count; row++) {
      EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, row), "/book" + numbered("", row) + ".epub") << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, row), "/book" + numbered("", count - 1 - row) + ".epub")
          << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, row), "/book" + numbered("", authorOrder[row]) + ".epub")
          << count << ':' << row;
    }
  }
}

TEST_F(LibraryBuilderTest, SortAllocationFailureProducesValidDegradedIndex) {
  fake::reset();
  for (unsigned i = 0; i < 513; i++) fake::add("/book" + numbered("", i) + ".txt");
  fake::failAlloc = 6;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Series));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_TRUE(stats.ranksDegraded);
  EXPECT_TRUE(stats.indexReplaced);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 513);
}

namespace {

struct ShelfGroup {
  std::string name;
  uint16_t bookCount = 0;
  uint16_t position = GROUP_POSITION_NONE;
};

bool groupOfPath(LibraryIndexFile& index, const std::string& path, ShelfGroup& out) {
  for (uint16_t ordinal = 0; ordinal < index.bookCount(); ordinal++) {
    ClixRecord record{};
    std::string candidate;
    if (!index.readRecord(ordinal, record) || !index.readPath(record, candidate) || candidate != path) continue;
    ClixGroupRef ref{};
    if (!index.readGroupRef(ordinal, ref) || ref.groupId == CLIX_GROUP_NONE) return false;
    out.position = ref.position;
    return index.readGroup(ref.groupId, out.name, out.bookCount);
  }
  ADD_FAILURE() << "no record for " << path;
  return false;
}

std::vector<std::string> shelfOrder(LibraryIndexFile& index) {
  std::vector<std::string> paths;
  paths.reserve(index.bookCount());
  for (uint16_t row = 0; row < index.bookCount(); row++) paths.push_back(pathAt(index, SortOrder::GroupAsc, row));
  return paths;
}

}  // namespace

TEST_F(LibraryBuilderTest, PublisherGroupsSortAlphabeticallyWithBooksInTitleOrderInside) {
  fake::add("/c.epub");
  fake::add("/d.epub");
  bookMetadata["/a.epub"] = {"Zeta", "Author", "", std::nullopt, "Tor", "", ""};
  bookMetadata["/b.epub"] = {"Beta", "Author", "", std::nullopt, "Penguin", "", ""};
  bookMetadata["/c.epub"] = {"Alpha", "Author", "", std::nullopt, "Tor", "", ""};
  bookMetadata["/d.epub"] = {"Mid", "Author", "", std::nullopt, "", "", ""};

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));

  EXPECT_EQ(stats.groups, 2);
  EXPECT_EQ(stats.grouped, 3);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.groupKind(), GroupKind::Publisher);
  EXPECT_EQ(index.groupCount(), 2);
  EXPECT_EQ(index.groupedCount(), 3);
  const std::vector<std::string> expected{"/b.epub", "/c.epub", "/a.epub", "/d.epub"};
  EXPECT_EQ(shelfOrder(index), expected);
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, "Tor");
  EXPECT_EQ(group.bookCount, 2);
  EXPECT_EQ(group.position, GROUP_POSITION_NONE);
  ASSERT_TRUE(groupOfPath(index, "/b.epub", group));
  EXPECT_EQ(group.name, "Penguin");
  EXPECT_EQ(group.bookCount, 1);
}

TEST_F(LibraryBuilderTest, PublisherSpellingsDifferingOnlyInCaseShareOneGroup) {
  bookMetadata["/a.epub"] = {"Alpha", "Author", "", std::nullopt, "TOR", "", ""};
  bookMetadata["/b.epub"] = {"Beta", "Author", "", std::nullopt, "tor", "", ""};

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.groupCount(), 1);
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/b.epub", group));
  EXPECT_EQ(group.bookCount, 2);
}

TEST_F(LibraryBuilderTest, LanguageSpellingsAreGroupedUnderOneNormalisedTag) {
  fake::add("/c.epub");
  fake::add("/d.epub");
  bookMetadata["/a.epub"] = {"A", "Author", "", std::nullopt, "", "EN", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "", std::nullopt, "", "en-US", ""};
  bookMetadata["/c.epub"] = {"C", "Author", "", std::nullopt, "", "eng", ""};
  bookMetadata["/d.epub"] = {"D", "Author", "", std::nullopt, "", "fr", ""};

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Language));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.groupKind(), GroupKind::Language);
  EXPECT_EQ(index.groupCount(), 2);
  EXPECT_EQ(index.groupedCount(), 4);
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/b.epub", group));
  EXPECT_EQ(group.name, "en");
  EXPECT_EQ(group.bookCount, 3);
  ASSERT_TRUE(groupOfPath(index, "/d.epub", group));
  EXPECT_EQ(group.name, "fr");
  EXPECT_EQ(group.bookCount, 1);
}

TEST_F(LibraryBuilderTest, SubjectGroupsTakeTheBookSubject) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"] = {"A", "Author", "", std::nullopt, "", "", "Fantasy"};
  bookMetadata["/b.epub"] = {"B", "Author", "", std::nullopt, "", "", "History"};
  bookMetadata["/c.epub"] = {"C", "Author", "", std::nullopt, "", "", "Fantasy"};

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Subject));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.groupKind(), GroupKind::Subject);
  EXPECT_EQ(index.groupCount(), 2);
  EXPECT_EQ(index.groupedCount(), 3);
  const std::vector<std::string> expected{"/a.epub", "/c.epub", "/b.epub"};
  EXPECT_EQ(shelfOrder(index), expected);
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/c.epub", group));
  EXPECT_EQ(group.name, "Fantasy");
  EXPECT_EQ(group.bookCount, 2);
  EXPECT_EQ(group.position, GROUP_POSITION_NONE);
}

TEST_F(LibraryBuilderTest, OnlySeriesCarriesAPosition) {
  bookMetadata["/a.epub"] = {"A", "Author", "Discworld", 2.0f, "", "", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "Discworld", 1.0f, "", "", ""};

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  const std::vector<std::string> expected{"/b.epub", "/a.epub"};
  EXPECT_EQ(shelfOrder(index), expected);
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, "Discworld");
  EXPECT_EQ(group.position, 200);
}

TEST_F(LibraryBuilderTest, KindNoneEmitsNoGroupTableEvenWithMetadataOn) {
  bookMetadata["/a.epub"] = {"B", "Author", "Discworld", 1.0f, "Tor", "en", "Fantasy"};
  bookMetadata["/b.epub"] = {"A", "Author", "Discworld", 2.0f, "Tor", "en", "Fantasy"};

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));

  EXPECT_EQ(stats.groups, 0);
  EXPECT_EQ(stats.grouped, 0);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 1);
  EXPECT_EQ(index.groupKind(), GroupKind::None);
  EXPECT_EQ(index.groupCount(), 0);
  EXPECT_EQ(index.groupedCount(), 0);
  const std::vector<std::string> expected{"/b.epub", "/a.epub"};
  EXPECT_EQ(shelfOrder(index), expected);
  ShelfGroup group;
  EXPECT_FALSE(groupOfPath(index, "/a.epub", group));
}

TEST_F(LibraryBuilderTest, MetadataOffEmitsKindNoneWhateverKindIsAsked) {
  for (const GroupKind kind : {GroupKind::Series, GroupKind::Publisher, GroupKind::Language, GroupKind::Subject}) {
    ASSERT_TRUE(buildLibraryIndex("/", stats, false, kind));
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    EXPECT_EQ(index.groupKind(), GroupKind::None) << static_cast<unsigned>(kind);
    EXPECT_EQ(index.groupCount(), 0);
  }
}

TEST_F(LibraryBuilderTest, RebuildWithTheSameKindReusesTheGroupAndPosition) {
  bookMetadata["/a.epub"] = {"A", "Author", "Discworld", 2.5f, "", "", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "Discworld", 1.0f, "", "", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_FALSE(stats.indexReplaced);

  // change one book to force a replacement; the other's group comes from the old index
  fake::files["/b.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
  EXPECT_TRUE(stats.indexReplaced);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, "Discworld");
  EXPECT_EQ(group.position, 250);
}

TEST_F(LibraryBuilderTest, ChangingTheKindReadsEveryBookAgain) {
  bookMetadata["/a.epub"] = {"A", "Author", "Discworld", 1.0f, "Tor", "EN", "Fantasy"};
  bookMetadata["/b.epub"] = {"B", "Author", "", std::nullopt, "Penguin", "fre", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  fake::parses = 0;

  // Only the configured field is stored, so a new kind cannot reuse the old index.
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    EXPECT_EQ(index.groupKind(), GroupKind::Publisher);
    EXPECT_EQ(index.groupCount(), 2);
    ShelfGroup group;
    ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
    EXPECT_EQ(group.name, "Tor");
    EXPECT_EQ(group.position, GROUP_POSITION_NONE);
  }

  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Language));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.groups, 2);
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    ShelfGroup group;
    ASSERT_TRUE(groupOfPath(index, "/b.epub", group));
    EXPECT_EQ(group.name, "fr");
  }
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Subject));
  EXPECT_EQ(stats.grouped, 1);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, "Discworld");
  EXPECT_EQ(group.position, 100);
}

TEST_F(LibraryBuilderTest, AnOverlongSeriesNameIsTruncatedAtTheStagingCap) {
  // Exceed the staging record so the stored value and heading are both shortened.
  const std::string series(600, 's');
  bookMetadata["/a.epub"] = {"A", "Author", series, 1.0f, "Tor", "en", "Fantasy"};
  bookMetadata["/b.epub"] = {"B", "Author", series, 2.0f, "Penguin", "", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(stats.groups, 1);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, std::string(CLIX_GROUP_NAME_BYTES, 's'));
  EXPECT_EQ(group.position, 100);
  std::string stored;
  uint16_t position = GROUP_POSITION_NONE;
  ClixRecord record{};
  ASSERT_TRUE(index.readRecord(index.ordinalForRow(SortOrder::TitleAsc, 0), record));
  ASSERT_TRUE(index.readSourceGroup(record, stored, &position));
  EXPECT_LT(stored.size(), series.size());
  EXPECT_GT(stored.size(), CLIX_GROUP_NAME_BYTES);
  EXPECT_EQ(position, 100);
}

TEST_F(LibraryBuilderTest, BookWithoutTheChosenFieldJoinsTheUngroupedTail) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"] = {"A", "Author", "Discworld", 1.0f, "", "", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "", std::nullopt, "Tor", "", ""};
  bookMetadata["/c.epub"] = {"C", "Author", "", std::nullopt, "Tor", "", ""};

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.groupedCount(), 2);
  const std::vector<std::string> expected{"/b.epub", "/c.epub", "/a.epub"};
  EXPECT_EQ(shelfOrder(index), expected);
  ShelfGroup group;
  EXPECT_FALSE(groupOfPath(index, "/a.epub", group));
}

TEST_F(LibraryBuilderTest, FullGroupNamesDistinguishMatchingHeadingsAndSurviveReuse) {
  const std::string prefix = "History / Europe / Great Britain / Social life and customs / ";
  ASSERT_EQ(prefix.size(), CLIX_GROUP_NAME_BYTES);
  bookMetadata["/a.epub"].subject = prefix + "19th century";
  bookMetadata["/b.epub"].subject = prefix + "18th century";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Subject));
  EXPECT_EQ(stats.groups, 2);

  fake::add("/c.epub");
  bookMetadata["/c.epub"].subject = prefix + "18th century";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Subject));
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_EQ(stats.parsed, 1);
  EXPECT_EQ(stats.groups, 2);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(shelfOrder(index), (std::vector<std::string>{"/b.epub", "/c.epub", "/a.epub"}));
  for (uint16_t row = 0; row < index.bookCount(); row++) {
    ClixRecord record{};
    std::string path;
    std::string source;
    ASSERT_TRUE(index.readRecord(row, record));
    ASSERT_TRUE(index.readPath(record, path));
    ASSERT_TRUE(index.readSourceGroup(record, source));
    EXPECT_EQ(source, bookMetadata[path].subject);
  }
  ClixGroupRef a{}, b{}, c{};
  ASSERT_TRUE(index.readGroupRef(0, a));
  ASSERT_TRUE(index.readGroupRef(1, b));
  ASSERT_TRUE(index.readGroupRef(2, c));
  EXPECT_NE(a.groupId, b.groupId);
  EXPECT_EQ(b.groupId, c.groupId);
}

TEST_F(LibraryBuilderTest, CommonGroupPrefixesSortLexicallyBeforeSeriesPosition) {
  // Cover 12-byte sort boundaries and prefixes near the 180-byte series cap.
  for (const size_t prefixLength : {0u, 11u, 12u, 13u, 23u, 24u, 36u, 39u, 60u, 120u, 170u}) {
    SCOPED_TRACE(prefixLength);
    fake::reset();
    bookMetadata.clear();
    const std::string prefix(prefixLength, 'X');
    const std::vector<std::string> groups{prefix + "Narnia", prefix + "Amber", prefix + "Amber", prefix, ""};
    for (unsigned i = 0; i < groups.size(); i++) {
      const std::string path = "/" + numbered("", i) + ".epub";
      fake::add(path);
      bookMetadata[path].series = groups[i];
      bookMetadata[path].seriesIndex = i == 2 ? 1.0f : 9.0f;
    }
    ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    const std::vector<std::string> expected =
        prefixLength == 0
            ? std::vector<std::string>{"/0002.epub", "/0001.epub", "/0000.epub", "/0003.epub", "/0004.epub"}
            : std::vector<std::string>{"/0003.epub", "/0002.epub", "/0001.epub", "/0000.epub", "/0004.epub"};
    EXPECT_EQ(shelfOrder(index), expected);
  }
}

TEST_F(LibraryBuilderTest, UngroupedBuildsUseCachedMetadataAndNoGroupStage) {
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(fake::plainLoads, 2u);
  EXPECT_EQ(fake::groupedLoads, 0u);
  EXPECT_EQ(fake::writesByPath.count(GROUP_STAGE), 0u);

  fake::reset();
  fake::add("/a.epub");
  fake::add("/b.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::plainLoads, 0u);
  EXPECT_EQ(fake::groupedLoads, 2u);
  EXPECT_GT(fake::writesByPath[GROUP_STAGE], 0u);
  EXPECT_FALSE(Storage.exists(GROUP_STAGE));
  EXPECT_FALSE(stats.groupsDegraded);
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_EQ(fake::parses, 0u);
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    EXPECT_EQ(index.groupKind(), GroupKind::None);
  }

  // an ungrouped index has no group values to reuse, so turning grouping on parses again
  bookMetadata["/a.epub"].series = "Dune";
  bookMetadata["/b.epub"].series = "Dune";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.groups, 1);
}

TEST_F(LibraryBuilderTest, GroupAllocationFailureIsExplicitAndCanBeRetried) {
  bookMetadata["/a.epub"].series = "Dune";
  bookMetadata["/b.epub"].series = "Dune";
  fake::failAllocationBytes = GROUP_SORT_KEY_BYTES * 2;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_TRUE(stats.groupsDegraded);
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    EXPECT_EQ(index.groupKind(), GroupKind::Series);
    EXPECT_TRUE(index.groupsDegraded());
    EXPECT_FALSE(index.hasGroups());
    EXPECT_EQ(index.groupCount(), 0);
    EXPECT_EQ(index.groupedCount(), 0);
    ClixRecord record{};
    std::string sourceGroup;
    ASSERT_TRUE(index.readRecord(0, record));
    ASSERT_TRUE(index.readSourceGroup(record, sourceGroup));
    EXPECT_TRUE(matchesQuery(fold(sourceGroup), "dune"));
  }
  fake::failAllocationBytes = 0;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_FALSE(stats.groupsDegraded);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_TRUE(stats.indexReplaced);
  EXPECT_EQ(stats.groups, 1);
  EXPECT_EQ(stats.grouped, 2);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_FALSE(index.groupsDegraded());
  EXPECT_TRUE(index.hasGroups());
}

TEST_F(LibraryBuilderTest, UnreadablePriorGroupReparsesOnlyThatBook) {
  bookMetadata["/a.epub"] = {"A", "Writer", "", std::nullopt, "Tor", "", ""};
  bookMetadata["/b.epub"] = {"B", "Writer", "", std::nullopt, "Penguin", "", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));

  // Corrupt /a.epub's group field length while leaving earlier fields intact.
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    for (uint16_t ordinal = 0; ordinal < index.bookCount(); ordinal++) {
      ClixRecord record{};
      std::string path, author, title, sourceAuthor;
      ASSERT_TRUE(index.readRecord(ordinal, record));
      ASSERT_TRUE(index.readPath(record, path));
      if (path != "/a.epub") continue;
      ASSERT_TRUE(index.readAuthor(record, author));
      ASSERT_TRUE(index.readTitle(record, title));
      ASSERT_TRUE(index.readSourceAuthor(record, sourceAuthor));
      // Skip the series position to reach the field length. A 255-byte
      // claim runs past the end of this two-book blob section.
      const size_t at = index.header().nameStart + record.nameOff + sizeof(uint64_t) + record.nameLen + 1 +
                        author.size() + 1 + title.size() + 1 + sourceAuthor.size() + sizeof(uint16_t);
      const uint8_t badLength = 0xFF;
      std::memcpy(fake::files[INDEX]->bytes.data() + at, &badLength, sizeof(badLength));
      ASSERT_FALSE(index.readSourceGroup(record, sourceAuthor));
    }
  }
  bookMetadata["/a.epub"].publisher = "Orbit";
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.parsed, 1);
  EXPECT_EQ(stats.metadataReused, 1);
  EXPECT_TRUE(stats.indexReplaced);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, "Orbit");
  ASSERT_TRUE(groupOfPath(index, "/b.epub", group));
  EXPECT_EQ(group.name, "Penguin");
}

TEST_F(LibraryBuilderTest, EveryStageReadAndSeekFailurePreservesPreviousIndex) {
  // Equal long prefixes exercise both the initial group scan and refinement.
  for (const char* const stagePath : {STAGE, GROUP_STAGE}) {
    for (const bool failSeeks : {false, true}) {
      SCOPED_TRACE(stagePath);
      unsigned triggered = 0;
      for (int failure = 0; failure < 300; failure++) {
        fake::reset();
        bookMetadata.clear();
        fake::add("/a.epub");
        fake::add("/b.epub");
        bookMetadata["/a.epub"].series = std::string(70, 'X') + "Narnia";
        bookMetadata["/b.epub"].series = std::string(70, 'X') + "Amber";
        ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
        const auto old = fake::files[INDEX]->bytes;
        fake::files["/a.epub"]->time++;
        fake::readFailurePath = stagePath;
        fake::seekFailurePath = stagePath;
        if (failSeeks)
          fake::failSeek = failure;
        else
          fake::failRead = failure;
        const bool result = buildLibraryIndex("/", stats, true, GroupKind::Series);
        if (!fake::failureTriggered) break;
        SCOPED_TRACE(::testing::Message() << "seek=" << failSeeks << " operation=" << failure);
        triggered++;
        EXPECT_FALSE(result);
        EXPECT_FALSE(stats.groupsDegraded);
        EXPECT_EQ(fake::files[INDEX]->bytes, old);
        EXPECT_FALSE(Storage.exists(STAGE));
        EXPECT_FALSE(Storage.exists(GROUP_STAGE));
      }
      EXPECT_GT(triggered, 10u);
      EXPECT_LT(triggered, 300u);
    }
  }
}

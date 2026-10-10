#include <LanguageTag.h>
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

TEST_F(LibraryBuilderTest, HeaderGroupKindFollowsMetadataMode) {
  initial();
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    EXPECT_EQ(index.groupKind(), GroupKind::Series);
  }

  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Series));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.groupKind(), GroupKind::None);
  EXPECT_EQ(index.groupCount(), 0);
  EXPECT_EQ(index.groupedCount(), 0);
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
  bookMetadata["/a.epub"] = {"Zeta", "Author", "", "", "Tor", "", ""};
  bookMetadata["/b.epub"] = {"Beta", "Author", "", "", "Penguin", "", ""};
  bookMetadata["/c.epub"] = {"Alpha", "Author", "", "", "Tor", "", ""};
  bookMetadata["/d.epub"] = {"Mid", "Author", "", "", "", "", ""};

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
  bookMetadata["/a.epub"] = {"Alpha", "Author", "", "", "TOR", "", ""};
  bookMetadata["/b.epub"] = {"Beta", "Author", "", "", "tor", "", ""};

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
  bookMetadata["/a.epub"] = {"A", "Author", "", "", "", "EN", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "", "", "", "en-US", ""};
  bookMetadata["/c.epub"] = {"C", "Author", "", "", "", "eng", ""};
  bookMetadata["/d.epub"] = {"D", "Author", "", "", "", "fr", ""};

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

TEST_F(LibraryBuilderTest, UnusableLanguageTagLeavesTheBookUngrouped) {
  bookMetadata["/a.epub"] = {"A", "Author", "", "", "", "und-", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "", "", "", "123", ""};

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Language));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.groupKind(), GroupKind::Language);
  ShelfGroup group;
  EXPECT_FALSE(groupOfPath(index, "/a.epub", group));
  EXPECT_FALSE(groupOfPath(index, "/b.epub", group));
}

TEST_F(LibraryBuilderTest, SubjectGroupsTakeTheBookSubject) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"] = {"A", "Author", "", "", "", "", "Fantasy"};
  bookMetadata["/b.epub"] = {"B", "Author", "", "", "", "", "History"};
  bookMetadata["/c.epub"] = {"C", "Author", "", "", "", "", "Fantasy"};

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
  bookMetadata["/a.epub"] = {"A", "Author", "Discworld", "2", "", "", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "Discworld", "1", "", "", ""};

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
  bookMetadata["/a.epub"] = {"B", "Author", "Discworld", "1", "Tor", "en", "Fantasy"};
  bookMetadata["/b.epub"] = {"A", "Author", "Discworld", "2", "Tor", "en", "Fantasy"};

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

TEST_F(LibraryBuilderTest, RebuildWithTheSameKindReusesMetadataAndCarriesTheGroup) {
  bookMetadata["/a.epub"] = {"A", "Author", "", "", "Tor", "", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "", "", "Penguin", "", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_FALSE(stats.indexReplaced);

  // Force a replacement with one changed book while reusing the other book's group fields.
  fake::files["/b.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
  EXPECT_TRUE(stats.indexReplaced);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, "Tor");
}

TEST_F(LibraryBuilderTest, ChangingTheKindReusesEveryBooksStoredFields) {
  bookMetadata["/a.epub"] = {"A", "Author", "Discworld", "1", "Tor", "EN", "Fantasy"};
  bookMetadata["/b.epub"] = {"B", "Author", "", "", "Penguin", "fre", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.metadataReused, 2);
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

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Language));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.groups, 2);
  {
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    ShelfGroup group;
    ASSERT_TRUE(groupOfPath(index, "/b.epub", group));
    EXPECT_EQ(group.name, "fr");
  }
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Subject));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.grouped, 1);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, "Discworld");
  EXPECT_EQ(group.position, 100);
}

TEST_F(LibraryBuilderTest, AnOverlongSeriesNameLeavesTheOtherStoredFieldsIntact) {
  // Exceed the full staging buffer to check that other fields retain their space.
  const std::string series(600, 's');
  bookMetadata["/a.epub"] = {"A", "Author", series, "1", "Tor", "en", "Fantasy"};
  bookMetadata["/b.epub"] = {"B", "Author", "", "", "Penguin", "", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.groups, 2);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ShelfGroup group;
  ASSERT_TRUE(groupOfPath(index, "/a.epub", group));
  EXPECT_EQ(group.name, "Tor");
  SourceFields fields;
  ClixRecord record{};
  ASSERT_TRUE(index.readRecord(index.ordinalForRow(SortOrder::TitleAsc, 0), record));
  ASSERT_TRUE(index.readSourceFields(record, fields));
  EXPECT_EQ(fields.field[groupFieldIndex(GroupKind::Language)], "en");
  EXPECT_EQ(fields.field[groupFieldIndex(GroupKind::Subject)], "Fantasy");
  EXPECT_LT(fields.field[groupFieldIndex(GroupKind::Series)].size(), series.size());
}

TEST_F(LibraryBuilderTest, TurningGroupingOffAndOnAgainParsesOnce) {
  bookMetadata["/a.epub"] = {"A", "Author", "", "", "Tor", "", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(stats.metadataReused, 2);
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));
  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.groups, 1);
}

TEST_F(LibraryBuilderTest, BookWithoutTheChosenFieldJoinsTheUngroupedTail) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"] = {"A", "Author", "Discworld", "1", "", "", ""};
  bookMetadata["/b.epub"] = {"B", "Author", "", "", "Tor", "", ""};
  bookMetadata["/c.epub"] = {"C", "Author", "", "", "Tor", "", ""};

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
      bookMetadata[path].seriesIndexText = i == 2 ? "1" : "9";
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

TEST_F(LibraryBuilderTest, LanguageAliasesOutsideHyphenationLanguagesShareAGroup) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].language = "nl";
  bookMetadata["/b.epub"].language = "nld";
  bookMetadata["/c.epub"].language = "dut";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Language));
  EXPECT_EQ(stats.groups, 1);
  EXPECT_EQ(stats.grouped, 3);
}

TEST_F(LibraryBuilderTest, DisablingGroupingUsesCachedMetadataAndSkipsGroupAllocations) {
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(fake::cachedLoads, 2u);
  EXPECT_EQ(fake::groupedLoads, 0u);
  EXPECT_EQ(fake::writesByPath.count(GROUP_STAGE), 0u);
  const auto plainAllocations = fake::allocationSizes;

  fake::reset();
  fake::add("/a.epub");
  fake::add("/b.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Series));
  EXPECT_EQ(fake::cachedLoads, 0u);
  EXPECT_EQ(fake::groupedLoads, 2u);
  // Grouping adds staging buffers, output arrays, sort keys and scratch buffers.
  EXPECT_EQ(fake::allocationSizes.size(), plainAllocations.size() + 9);
  EXPECT_GT(fake::writesByPath[GROUP_STAGE], 0u);
  EXPECT_FALSE(Storage.exists(GROUP_STAGE));
  EXPECT_FALSE(stats.groupsDegraded);
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.groupKind(), GroupKind::None);
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

TEST_F(LibraryBuilderTest, UngroupedBuildNeverCreatesTheGroupStage) {
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  ASSERT_TRUE(buildLibraryIndex("/", stats, false, GroupKind::Series));

  EXPECT_EQ(fake::writesByPath.count(GROUP_STAGE), 0u);
  EXPECT_FALSE(Storage.exists(GROUP_STAGE));
  EXPECT_FALSE(Storage.exists(STAGE));
}

TEST_F(LibraryBuilderTest, UnreadablePriorGroupReparsesOnlyThatBook) {
  bookMetadata["/a.epub"] = {"A", "Writer", "", "", "Tor", "", ""};
  bookMetadata["/b.epub"] = {"B", "Writer", "", "", "Penguin", "", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Publisher));

  // Corrupt /a.epub's series length while leaving earlier fields intact.
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
      // Skip the series position to reach the first field length. A 255-byte
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

TEST_F(LibraryBuilderTest, NoGroupingBypassesReaderCacheForChangedOrUndatedBooks) {
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(fake::cachedLoads, 2u);
  EXPECT_EQ(fake::bypassLoads, 0u);
  fake::files["/a.epub"]->time++;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(stats.metadataReused, 1);
  EXPECT_EQ(fake::cachedLoads, 3u);
  EXPECT_EQ(fake::bypassLoads, 1u);
  fake::files["/b.epub"]->bytes.push_back('x');
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(fake::cachedLoads, 4u);
  EXPECT_EQ(fake::bypassLoads, 2u);
  fake::files["/a.epub"]->time = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::None));
  EXPECT_EQ(fake::cachedLoads, 5u);
  EXPECT_EQ(fake::bypassLoads, 3u);
  EXPECT_EQ(fake::groupedLoads, 0u);
}

TEST_F(LibraryBuilderTest, GroupAllocationFailurePreservesSearchableLanguageMetadata) {
  bookMetadata["/a.epub"].language = "fra";
  bookMetadata["/b.epub"].language = "eng";
  fake::failAllocationBytes = GROUP_SORT_KEY_BYTES * 2;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, GroupKind::Language));
  ASSERT_TRUE(stats.groupsDegraded);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_FALSE(index.hasGroups());
  ClixRecord record{};
  std::string sourceGroup;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readSourceGroup(record, sourceGroup));
  EXPECT_EQ(sourceGroup, "fr");
  const char* name = languageNameForTag(sourceGroup.c_str());
  ASSERT_NE(name, nullptr);
  EXPECT_TRUE(matchesQuery(fold(name), "francais"));
}

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

using namespace library;

namespace {

constexpr char INDEX[] = "/.crosspoint/library.idx";

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

  void initial() { ASSERT_TRUE(buildLibraryIndex("/", stats, true)); }
};

}  // namespace

TEST_F(LibraryBuilderTest, UnchangedRebuildReusesMetadataAndDoesNotReplaceIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

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
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "initial build failed for " << count << " books";
      return 0u;
    }
    fake::resetIoCounters();
    if (!buildLibraryIndex("/", stats, false)) {
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

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(fake::directoryEntriesByPath["/a.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/b.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder/c.txt"], 1u);
}

TEST_F(LibraryBuilderTest, StagingAndIndexWritesAreBatched) {
  fake::reset();
  for (unsigned i = 0; i < 128; i++) fake::add("/book" + numbered("", i) + ".txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_LT(fake::writesByPath["/.crosspoint/library.stage"], 64u);
  EXPECT_LT(fake::writesByPath["/.crosspoint/library.new"], 32u);
}

TEST_F(LibraryBuilderTest, ParentDuplicateTrackingSurvivesDirectoryRecursion) {
  fake::add("/folder/c.txt");
  fake::duplicateDirectoryEntry("/a.epub");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(stats.books, 3);
  EXPECT_EQ(stats.duplicatesDropped, 1);
}

TEST_F(LibraryBuilderTest, TimestampAndSizeChangesParseOnlyTheChangedBook) {
  initial();
  fake::files["/a.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);

  fake::files["/b.epub"]->bytes.push_back('x');
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
}

TEST_F(LibraryBuilderTest, ZeroTimestampAndFailedExtractionAreNeverFresh) {
  fake::files["/a.epub"]->time = 0;
  bookMetadata["/b.epub"].success = false;
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);
}

TEST_F(LibraryBuilderTest, MetadataModeChangesInvalidateCachedMetadata) {
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
  index.close();

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
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

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

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

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 3);
}

TEST_F(LibraryBuilderTest, AddedRemovedMovedAndRenamedBooksKeepArrivalOrder) {
  initial();
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, 1), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, 2), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.rename("/a.epub", "/moved.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, 0), "/moved.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, 1), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.rename("/moved.epub", "/renamed.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, 0), "/renamed.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, 1), "/c.epub");
}

TEST_F(LibraryBuilderTest, WholeFolderRenameWithUniqueSizePreservesArrivalOrder) {
  fake::add("/old/unique.epub", "a uniquely sized book");
  initial();
  ASSERT_TRUE(Storage.mkdir("/new"));
  ASSERT_TRUE(Storage.rename("/old/unique.epub", "/new/unique.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(stats.renamed, 1);
  EXPECT_EQ(stats.removed, 0);
  EXPECT_EQ(fake::parses, 1u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, 2), "/new/unique.epub");
}

TEST_F(LibraryBuilderTest, DuplicateDetectionRemainsBoundedAndFindsTrackedKeysAfterTheCap) {
  fake::duplicateDirectoryEntry("/a.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.books, 2);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_FALSE(stats.dedupDegraded);

  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i <= LIBRARY_MAX_DEDUP_KEYS; i++) {
    fake::add("/book" + numbered("", i) + ".txt");
  }
  fake::duplicateDirectoryEntry("/book0000.txt");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.books, LIBRARY_MAX_DEDUP_KEYS + 1);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_TRUE(stats.dedupDegraded);
  EXPECT_LT(fake::delays, 2000u);
}

TEST_F(LibraryBuilderTest, ReadWriteCloseAndAllocationFailuresRetainPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;

  fake::failRead = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failRead = -1;

  fake::files["/a.epub"]->time++;
  fake::failWrite = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failWrite = -1;

  fake::failWritePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failClosePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failAlloc = 3;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failAlloc = -1;

  fake::failRename = 1;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
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

  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
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

    ASSERT_TRUE(buildLibraryIndex("/", stats, true)) << count;
    ASSERT_EQ(stats.books, count);
    EXPECT_FALSE(stats.ranksDegraded);

    if (count == CLIX_MAX_RECORDS) {
      const auto old = fake::files[INDEX]->bytes;
      fake::parses = 0;
      fake::resetIoCounters();
      ASSERT_TRUE(buildLibraryIndex("/", stats, true));
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
      EXPECT_EQ(pathAt(index, SortOrder::AddedAsc, row), "/book" + numbered("", row) + ".epub") << count << ':' << row;
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

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_TRUE(stats.ranksDegraded);
  EXPECT_TRUE(stats.indexReplaced);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 513);
}

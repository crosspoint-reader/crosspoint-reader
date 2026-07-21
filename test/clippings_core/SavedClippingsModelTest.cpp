#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "activities/reader/SavedClippingsModel.h"

namespace {

ClippingStore::CatalogEntry entry(std::string title, const uint32_t timestamp,
                                  const ClippingCodec::Format format = ClippingCodec::Format::Current) {
  ClippingStore::CatalogEntry value;
  value.book.title = std::move(title);
  value.book.path = "/books/" + value.book.title + ".epub";
  value.format = format;
  value.newestTimestamp = timestamp;
  value.clippingCount = 1;
  return value;
}

TEST(SavedClippingsModelTest, SortsKnownCurrentTimestampsNewestFirstThenUnknownBooksByTitle) {
  std::vector<ClippingStore::CatalogEntry> entries{
      entry("Zulu", 0),
      entry("Older", 1700000000U),
      entry("Legacy", 1900000000U, ClippingCodec::Format::CrossInkV2),
      entry("Newest", 1800000000U),
      entry("Future", 4200000000U),
      entry("Alpha", 0),
  };

  SavedClippingsModel::sortEntries(entries);

  ASSERT_EQ(entries.size(), 6U);
  EXPECT_EQ(entries[0].book.title, "Newest");
  EXPECT_EQ(entries[1].book.title, "Older");
  EXPECT_EQ(entries[2].book.title, "Alpha");
  EXPECT_EQ(entries[3].book.title, "Future");
  EXPECT_EQ(entries[4].book.title, "Legacy");
  EXPECT_EQ(entries[5].book.title, "Zulu");
}

TEST(SavedClippingsModelTest, KeepsUntitledUnknownBooksAfterNamedBooks) {
  std::vector<ClippingStore::CatalogEntry> entries{entry("", 0), entry("Named", 0)};

  SavedClippingsModel::sortEntries(entries);

  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].book.title, "Named");
  EXPECT_TRUE(entries[1].book.title.empty());
}

TEST(SavedClippingsModelTest, DistinguishesEmptyIncompleteAndReadFailure) {
  ClippingStore::Catalog catalog;
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::DirectoryMissing, catalog),
            SavedClippingsModel::CatalogState::Empty);
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::IoError, catalog),
            SavedClippingsModel::CatalogState::ReadError);

  catalog.entryNameTruncated = true;
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::Loaded, catalog),
            SavedClippingsModel::CatalogState::Incomplete);
  EXPECT_FALSE(SavedClippingsModel::canExport(ClippingStore::CatalogLoadResult::Loaded, catalog));

  catalog = {};
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::Loaded, catalog),
            SavedClippingsModel::CatalogState::Empty);
  catalog.entries.push_back(entry("Book", 1700000000U));
  EXPECT_EQ(SavedClippingsModel::state(ClippingStore::CatalogLoadResult::Loaded, catalog),
            SavedClippingsModel::CatalogState::Ready);
  EXPECT_TRUE(SavedClippingsModel::canExport(ClippingStore::CatalogLoadResult::Loaded, catalog));
}

TEST(SavedClippingsModelTest, EveryBoundedOrSkippedCatalogIsIncomplete) {
  ClippingStore::Catalog catalog;
  catalog.entries.push_back(entry("Book", 1700000000U));

  catalog.directoryTruncated = true;
  EXPECT_FALSE(SavedClippingsModel::isComplete(ClippingStore::CatalogLoadResult::Loaded, catalog));
  catalog.directoryTruncated = false;
  catalog.entryNameTruncated = true;
  EXPECT_FALSE(SavedClippingsModel::isComplete(ClippingStore::CatalogLoadResult::Loaded, catalog));
  catalog.entryNameTruncated = false;
  catalog.skippedBooks = 1;
  EXPECT_FALSE(SavedClippingsModel::isComplete(ClippingStore::CatalogLoadResult::Loaded, catalog));
}

}  // namespace

#include <gtest/gtest.h>

#include <cstring>
#include <utility>
#include <vector>

#include "LibraryIndexFile.h"

namespace library {

std::string joinLibraryPath(const std::string_view folder, const std::string_view name) {
  return std::string(folder) + "/" + std::string(name);
}

}  // namespace library

namespace {

std::vector<uint8_t> makeBlob(const uint64_t pathHash, const std::initializer_list<uint8_t> fields) {
  std::vector<uint8_t> blob(sizeof(pathHash) + fields.size());
  std::memcpy(blob.data(), &pathHash, sizeof(pathHash));
  std::copy(fields.begin(), fields.end(), blob.begin() + sizeof(pathHash));
  return blob;
}

}  // namespace

TEST(LibraryIndexFile, MissingIndexDoesNotCloseAnUninitializedHandle) {
  Storage.clearFile();
  HalFile::resetInvalidCloseCount();

  {
    library::LibraryIndexFile index;
    EXPECT_FALSE(index.open("/missing.clx"));
  }

  EXPECT_EQ(HalFile::invalidCloseCount(), 0);
}

TEST(LibraryIndexFile, ReadsEveryStoredOrderInBothDirections) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 3;
  library::layoutSections(header, 0, 0);

  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  const uint16_t authorOrder[] = {2, 0, 1};
  const uint16_t arrivalOrder[] = {1, 2, 0};
  std::memcpy(bytes.data() + library::authorOrderOffset(header, 0), authorOrder, sizeof(authorOrder));
  std::memcpy(bytes.data() + library::arrivalOrderOffset(header, 0), arrivalOrder, sizeof(arrivalOrder));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));

  const auto expectOrder = [&](const library::SortOrder order, const uint16_t a, const uint16_t b, const uint16_t c) {
    EXPECT_EQ(index.ordinalForRow(order, 0), a);
    EXPECT_EQ(index.ordinalForRow(order, 1), b);
    EXPECT_EQ(index.ordinalForRow(order, 2), c);
    EXPECT_EQ(index.ordinalForRow(order, 3), 0xFFFF);
  };
  expectOrder(library::SortOrder::RecentAsc, 1, 2, 0);
  expectOrder(library::SortOrder::RecentDesc, 0, 2, 1);
  expectOrder(library::SortOrder::TitleAsc, 0, 1, 2);
  expectOrder(library::SortOrder::TitleDesc, 2, 1, 0);
  expectOrder(library::SortOrder::AuthorAsc, 2, 0, 1);
  expectOrder(library::SortOrder::AuthorDesc, 1, 0, 2);
}

TEST(LibraryIndexFile, GroupKindAndGroupTableRoundTripThroughTheHeader) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 2;
  header.metadataEnabled = 1;
  header.groupKind = static_cast<uint8_t>(library::GroupKind::Series);
  header.groupCount = 1;
  header.groupedCount = 1;
  library::layoutSections(header, 0, 0);

  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  library::ClixGroupEntry entry{};
  entry.bookCount = 1;
  entry.nameLen = 5;
  std::memcpy(entry.name, "Dune!", entry.nameLen);
  std::memcpy(bytes.data() + library::groupEntryOffset(header, 0), &entry, sizeof(entry));
  const library::ClixGroupRef refs[] = {{0, 150}, {library::CLIX_GROUP_NONE, library::GROUP_POSITION_NONE}};
  std::memcpy(bytes.data() + library::groupRefOffset(header, 0), refs, sizeof(refs));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_EQ(index.groupKind(), library::GroupKind::Series);
  EXPECT_EQ(index.groupCount(), 1);
  EXPECT_EQ(index.groupedCount(), 1);
  EXPECT_TRUE(index.hasGroups());

  library::ClixGroupRef ref{};
  ASSERT_TRUE(index.readGroupRef(0, ref));
  EXPECT_EQ(ref.groupId, 0);
  EXPECT_EQ(ref.position, 150);
  ASSERT_TRUE(index.readGroupRef(1, ref));
  EXPECT_EQ(ref.groupId, library::CLIX_GROUP_NONE);
  EXPECT_EQ(ref.position, library::GROUP_POSITION_NONE);

  std::string name;
  uint16_t books = 0;
  ASSERT_TRUE(index.readGroup(0, name, books));
  EXPECT_EQ(name, "Dune!");
  EXPECT_EQ(books, 1);
  EXPECT_FALSE(index.readGroup(1, name, books));
}

TEST(LibraryIndexFile, AnIndexWithoutAGroupTableReportsKindNone) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  library::layoutSections(header, 0, 0);

  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_EQ(index.groupKind(), library::GroupKind::None);
  EXPECT_FALSE(index.hasGroups());
  index.close();
  EXPECT_EQ(index.groupKind(), library::GroupKind::None);
}

TEST(LibraryIndexFile, ResolvesRecentRowsByIdentity) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 3;
  // One 8-byte path hash blob per record.
  library::layoutSections(header, 0, 3 * sizeof(uint64_t));
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));

  // Ordinals 0 and 2 share a size, so only the hash can tell them apart.
  constexpr uint64_t HASHES[] = {11, 22, 33};
  constexpr uint32_t SIZES[] = {100, 200, 100};
  for (uint16_t ordinal = 0; ordinal < 3; ordinal++) {
    library::ClixRecord record{};
    record.fileSize = SIZES[ordinal];
    record.nameOff = ordinal * sizeof(uint64_t);
    std::memcpy(bytes.data() + library::recordOffset(header, ordinal), &record, sizeof(record));
    std::memcpy(bytes.data() + header.nameStart + record.nameOff, &HASHES[ordinal], sizeof(uint64_t));
  }
  const uint16_t arrivalOrder[] = {1, 2, 0};
  std::memcpy(bytes.data() + library::arrivalOrderOffset(header, 0), arrivalOrder, sizeof(arrivalOrder));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  const library::BookIdentity books[] = {
      {HASHES[0], SIZES[0]},  // ordinal 0 -> ascending row 2
      {HASHES[2], SIZES[2]},  // same size as ordinal 0, hash picks ordinal 2 -> row 1
      {99, SIZES[0]},         // size matches, hash does not: absent
      {HASHES[1], 999},       // hash matches, size does not: absent
      {HASHES[1], 0},         // size unknown: the hash alone matches -> row 0
  };
  uint16_t rows[5] = {};
  ASSERT_TRUE(index.recentRowsFor(books, 5, rows));
  EXPECT_EQ(rows[0], 2);
  EXPECT_EQ(rows[1], 1);
  EXPECT_EQ(rows[2], 0xFFFF);
  EXPECT_EQ(rows[3], 0xFFFF);
  EXPECT_EQ(rows[4], 0);
}

TEST(LibraryIndexFile, RejectsInvalidPermutationOrdinal) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  library::layoutSections(header, 0, 0);
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  const uint16_t invalid = 1;
  std::memcpy(bytes.data() + library::authorOrderOffset(header, 0), &invalid, sizeof(invalid));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_EQ(index.ordinalForRow(library::SortOrder::AuthorAsc, 0), 0xFFFF);
}

TEST(LibraryIndexFile, RejectsInvalidGroupPermutationOrdinal) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  header.metadataEnabled = 1;
  header.groupKind = static_cast<uint8_t>(library::GroupKind::Series);
  library::layoutSections(header, 0, 0);
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  const uint16_t invalid = 1;
  std::memcpy(bytes.data() + library::groupOrderOffset(header, 0), &invalid, sizeof(invalid));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_EQ(index.ordinalForRow(library::SortOrder::GroupAsc, 0), 0xFFFF);
  EXPECT_EQ(index.ordinalForRow(library::SortOrder::GroupDesc, 0), 0xFFFF);
}

TEST(LibraryIndexFile, ReadsPathHashAndEveryPublicBlobField) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  const uint8_t folder[] = {6, '/', 'b', 'o', 'o', 'k', 's'};
  constexpr uint64_t PATH_HASH = 0x0123456789ABCDEFULL;
  const auto blob = makeBlob(PATH_HASH, {'x', 1, 'a', 1, 't', 8, 'O', 'r', 'i', 'g', 'i', 'n', 'a', 'l'});
  header.folderCount = 1;
  library::layoutSections(header, sizeof(folder), blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folder, sizeof(folder));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  Storage.setFile("/library.clx", bytes);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  uint64_t pathHash = 0;
  ASSERT_TRUE(index.readPathHash(record, pathHash));
  EXPECT_EQ(pathHash, PATH_HASH);
  std::string name;
  ASSERT_TRUE(index.readName(record, name));
  EXPECT_EQ(name, "x");
  std::string author;
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "a");
  std::string title;
  ASSERT_TRUE(index.readTitle(record, title));
  EXPECT_EQ(title, "t");
  ASSERT_TRUE(index.readSourceAuthor(record, author));
  EXPECT_EQ(author, "Original");
  std::string path;
  ASSERT_TRUE(index.readPath(record, path));
  EXPECT_EQ(path, "/books/x");
  index.close();

  bytes[header.nameStart + sizeof(PATH_HASH) + 5] = 255;
  Storage.setFile("/library.clx", std::move(bytes));
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_FALSE(index.readSourceAuthor(record, author));
}

TEST(LibraryIndexFile, RejectsTruncatedAndOverflowingPathHashes) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  library::layoutSections(header, 0, sizeof(uint64_t) - 1);
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  library::ClixRecord record{};
  uint64_t hash = 1;
  EXPECT_FALSE(index.readPathHash(record, hash));
  EXPECT_EQ(hash, 0u);
  EXPECT_TRUE(index.ioFailed());

  record.nameOff = UINT32_MAX;
  EXPECT_FALSE(index.readPathHash(record, hash));
}

TEST(LibraryIndexFile, RejectsFolderRecordBeyondFolderBlob) {
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  const uint8_t folder[] = {5, '/'};
  const auto blob = makeBlob(1, {'x', 0, 0, 0});
  library::layoutSections(header, sizeof(folder), blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folder, sizeof(folder));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string path;
  EXPECT_FALSE(index.readPath(record, path));
}

TEST(LibraryIndexFile, SourceGroupRoundTripsFullBoundedValuesAndRejectsTruncatedBlobs) {
  for (const uint8_t storedLength : {0, 1, 255}) {
    for (const bool truncate : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "length=" << int{storedLength} << " truncate=" << truncate);
      // Series position, a series field of storedLength bytes, then three empty fields.
      auto blob = makeBlob(123, {'x', 1, 'a', 1, 't', 1, 's', 0xFF, 0xFF});
      const size_t lengthOffset = blob.size();
      blob.resize(blob.size() + sizeof(storedLength) + storedLength, 'G');
      std::memcpy(blob.data() + lengthOffset, &storedLength, sizeof(storedLength));
      for (int i = 0; i < 3; i++) blob.push_back(0);
      if (truncate) blob.resize(lengthOffset + sizeof(storedLength) + storedLength - 1);
      library::ClixHeader header{};
      std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
      header.formatVersion = library::CLIX_FORMAT_VERSION;
      header.foldVersion = library::CLIX_FOLD_VERSION;
      header.bookCount = 1;
      header.metadataEnabled = 1;
      header.groupKind = static_cast<uint8_t>(library::GroupKind::Series);
      library::layoutSections(header, 0, blob.size());
      std::vector<uint8_t> bytes(header.selfSize, 0);
      std::memcpy(bytes.data(), &header, sizeof(header));
      std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
      Storage.setFile("/library.clx", std::move(bytes));
      library::LibraryIndexFile index;
      ASSERT_TRUE(index.open("/library.clx"));
      library::ClixRecord record{};
      record.nameLen = 1;
      std::string group;
      const bool valid = !truncate;
      EXPECT_EQ(index.readSourceGroup(record, group), valid);
      if (valid)
        EXPECT_EQ(group, std::string(storedLength, 'G'));
      else
        EXPECT_TRUE(group.empty());
    }
  }
}

namespace {

void appendShortField(std::vector<uint8_t>& blob, const std::string& value) {
  blob.push_back(static_cast<uint8_t>(value.size()));
  blob.insert(blob.end(), value.begin(), value.end());
}

void appendU16(std::vector<uint8_t>& blob, const uint16_t value) {
  blob.push_back(static_cast<uint8_t>(value & 0xFF));
  blob.push_back(static_cast<uint8_t>(value >> 8));
}

// Series position, then series, publisher, language and subject fields.
void appendSourceFields(std::vector<uint8_t>& blob, const uint16_t position,
                        const std::initializer_list<std::string> fields) {
  appendU16(blob, position);
  for (const std::string& value : fields) {
    blob.push_back(static_cast<uint8_t>(value.size()));
    blob.insert(blob.end(), value.begin(), value.end());
  }
}

// Single-record index with a path hash, filename "x" and the supplied blob tail.
void storeIndexWithTail(const std::vector<uint8_t>& tail, const library::GroupKind kind = library::GroupKind::Series) {
  auto blob = makeBlob(42, {'x'});
  blob.insert(blob.end(), tail.begin(), tail.end());
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 1;
  header.metadataEnabled = kind == library::GroupKind::None ? 0 : 1;
  header.groupKind = static_cast<uint8_t>(kind);
  library::layoutSections(header, 0, blob.size());
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  Storage.setFile("/library.clx", std::move(bytes));
}

library::ClixRecord filenameRecord() {
  library::ClixRecord record{};
  record.nameLen = 1;
  return record;
}

}  // namespace

TEST(LibraryIndexFile, SearchFieldsReadsTitleAuthorAndSourceGroupInOnePass) {
  std::vector<uint8_t> tail;
  appendShortField(tail, "Author");
  appendShortField(tail, "Title");
  appendShortField(tail, "Source Author");
  appendSourceFields(tail, 250, {"Series", "Pub", "en", "Subj"});
  storeIndexWithTail(tail);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string title;
  std::string author;
  std::string group;
  EXPECT_TRUE(index.readSearchFields(filenameRecord(), &title, author, group));
  EXPECT_EQ(title, "Title");
  EXPECT_EQ(author, "Author");
  EXPECT_EQ(group, "Series");
}

TEST(LibraryIndexFile, SearchFieldsLeavesEmptyTitleAndAuthorEmpty) {
  std::vector<uint8_t> tail;
  appendShortField(tail, "");
  appendShortField(tail, "");
  appendShortField(tail, "");
  appendSourceFields(tail, library::GROUP_POSITION_NONE, {"", "Publisher", "", ""});
  storeIndexWithTail(tail, library::GroupKind::Publisher);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string title = "stale";
  std::string author = "stale";
  std::string group;
  EXPECT_TRUE(index.readSearchFields(filenameRecord(), &title, author, group));
  EXPECT_TRUE(title.empty());
  EXPECT_TRUE(author.empty());
  EXPECT_EQ(group, "Publisher");
  EXPECT_FALSE(index.readTitle(filenameRecord(), title));
  EXPECT_FALSE(index.readAuthor(filenameRecord(), author));
}

TEST(LibraryIndexFile, SearchFieldsSucceedsWithEmptySourceGroup) {
  std::vector<uint8_t> tail;
  appendShortField(tail, "Author");
  appendShortField(tail, "Title");
  appendShortField(tail, "");
  appendSourceFields(tail, library::GROUP_POSITION_NONE, {"", "", "", ""});
  storeIndexWithTail(tail);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string title;
  std::string author;
  std::string group = "stale";
  EXPECT_TRUE(index.readSearchFields(filenameRecord(), &title, author, group));
  EXPECT_EQ(title, "Title");
  EXPECT_EQ(author, "Author");
  EXPECT_TRUE(group.empty());
}

TEST(LibraryIndexFile, SearchFieldsKeepsTitleAndAuthorWhenSourceGroupIsMissing) {
  std::vector<uint8_t> tail;
  appendShortField(tail, "Author");
  appendShortField(tail, "Title");
  appendShortField(tail, "Source Author");
  storeIndexWithTail(tail);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string title;
  std::string author;
  std::string group;
  EXPECT_FALSE(index.readSearchFields(filenameRecord(), &title, author, group));
  EXPECT_EQ(title, "Title");
  EXPECT_EQ(author, "Author");
  EXPECT_TRUE(group.empty());
  EXPECT_FALSE(index.readSourceGroup(filenameRecord(), group));
}

TEST(LibraryIndexFile, SearchFieldsMatchesTheSingleFieldReadersForFieldsLongerThanTheWindow) {
  std::vector<uint8_t> tail;
  appendShortField(tail, std::string(255, 'a'));
  appendShortField(tail, std::string(200, 't'));
  appendShortField(tail, std::string(255, 's'));
  appendSourceFields(tail, 100, {"s", "p", "l", std::string(library::CLIX_SOURCE_GROUP_BYTES, 'g')});
  storeIndexWithTail(tail, library::GroupKind::Subject);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string title;
  std::string author;
  std::string group;
  ASSERT_TRUE(index.readSearchFields(filenameRecord(), &title, author, group));
  std::string expected;
  ASSERT_TRUE(index.readTitle(filenameRecord(), expected));
  EXPECT_EQ(title, expected);
  ASSERT_TRUE(index.readAuthor(filenameRecord(), expected));
  EXPECT_EQ(author, expected);
  ASSERT_TRUE(index.readSourceGroup(filenameRecord(), expected));
  EXPECT_EQ(group, expected);
  EXPECT_EQ(group.size(), library::CLIX_SOURCE_GROUP_BYTES);
}

TEST(LibraryIndexFile, SearchFieldsRejectsTheSameSourceGroupLengthsAsTheSingleFieldReader) {
  for (const uint8_t storedLength : {0, 1, 255}) {
    for (const bool truncate : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "length=" << int{storedLength} << " truncate=" << truncate);
      std::vector<uint8_t> tail;
      appendShortField(tail, "a");
      appendShortField(tail, "t");
      appendShortField(tail, "s");
      appendU16(tail, library::GROUP_POSITION_NONE);
      const size_t lengthOffset = tail.size();
      tail.resize(tail.size() + sizeof(storedLength) + storedLength, 'G');
      std::memcpy(tail.data() + lengthOffset, &storedLength, sizeof(storedLength));
      for (int i = 0; i < 3; i++) tail.push_back(0);
      if (truncate) tail.resize(lengthOffset + sizeof(storedLength) + storedLength - 1);
      storeIndexWithTail(tail);

      library::LibraryIndexFile index;
      ASSERT_TRUE(index.open("/library.clx"));
      std::string title;
      std::string author;
      std::string group;
      std::string expected;
      EXPECT_EQ(index.readSearchFields(filenameRecord(), &title, author, group),
                index.readSourceGroup(filenameRecord(), expected));
      EXPECT_EQ(group, expected);
      EXPECT_EQ(title, "t");
      EXPECT_EQ(author, "a");
    }
  }
}

TEST(LibraryIndexFile, SourceFieldsRoundTripEveryStoredFieldAndThePosition) {
  std::vector<uint8_t> tail;
  appendShortField(tail, "Author");
  appendShortField(tail, "Title");
  appendShortField(tail, "Source Author");
  appendSourceFields(tail, 350, {"Discworld", "Gollancz", "en", "Fantasy"});
  storeIndexWithTail(tail, library::GroupKind::Language);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  library::SourceFields fields;
  ASSERT_TRUE(index.readSourceFields(filenameRecord(), fields));
  EXPECT_EQ(fields.seriesPosition, 350);
  EXPECT_EQ(fields.field[library::groupFieldIndex(library::GroupKind::Series)], "Discworld");
  EXPECT_EQ(fields.field[library::groupFieldIndex(library::GroupKind::Publisher)], "Gollancz");
  EXPECT_EQ(fields.field[library::groupFieldIndex(library::GroupKind::Language)], "en");
  EXPECT_EQ(fields.field[library::groupFieldIndex(library::GroupKind::Subject)], "Fantasy");
  std::string group;
  ASSERT_TRUE(index.readSourceGroup(filenameRecord(), group));
  EXPECT_EQ(group, "en");
}

TEST(LibraryIndexFile, SourceFieldsFailOnATruncatedLaterField) {
  std::vector<uint8_t> tail;
  appendShortField(tail, "Author");
  appendShortField(tail, "Title");
  appendShortField(tail, "Source Author");
  appendSourceFields(tail, 350, {"Discworld", "Gollancz", "en", "Fantasy"});
  tail.pop_back();
  storeIndexWithTail(tail, library::GroupKind::Series);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  library::SourceFields fields;
  EXPECT_FALSE(index.readSourceFields(filenameRecord(), fields));
  // The configured field precedes the damage and remains readable.
  std::string group;
  ASSERT_TRUE(index.readSourceGroup(filenameRecord(), group));
  EXPECT_EQ(group, "Discworld");
}

TEST(LibraryIndexFile, AnIndexOfKindNoneHasAnEmptySourceGroup) {
  std::vector<uint8_t> tail;
  appendShortField(tail, "Author");
  appendShortField(tail, "Title");
  appendShortField(tail, "Source Author");
  appendSourceFields(tail, library::GROUP_POSITION_NONE, {"", "", "", ""});
  storeIndexWithTail(tail, library::GroupKind::None);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string group = "stale";
  EXPECT_TRUE(index.readSourceGroup(filenameRecord(), group));
  EXPECT_TRUE(group.empty());
  std::string title, author;
  group = "stale";
  EXPECT_TRUE(index.readSearchFields(filenameRecord(), &title, author, group));
  EXPECT_TRUE(group.empty());
}

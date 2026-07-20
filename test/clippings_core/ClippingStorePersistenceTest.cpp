#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ClippingCodec.h"
#include "ClippingStore.h"

namespace {

void appendU16(std::vector<uint8_t>& bytes, const uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void appendU32(std::vector<uint8_t>& bytes, const uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void appendString(std::vector<uint8_t>& bytes, const std::string& text) {
  appendU32(bytes, static_cast<uint32_t>(text.size()));
  bytes.insert(bytes.end(), text.begin(), text.end());
}

ClippingCodec::ClippingMetadata sampleClipping(const uint32_t timestamp = 42) {
  ClippingCodec::ClippingMetadata clipping;
  clipping.spineIndex = 1;
  clipping.startPage = 2;
  clipping.endPage = 2;
  clipping.pageCount = 5;
  clipping.startWordIndex = 4;
  clipping.endWordIndex = 6;
  clipping.wordCount = 3;
  clipping.timestamp = timestamp;
  clipping.chapterTitle = "Chương thử";
  return clipping;
}

std::vector<uint8_t> makeLegacy(const uint8_t version, const std::string& bookPath, const std::string& text) {
  const auto clipping = sampleClipping();
  std::vector<uint8_t> bytes{version};
  appendU16(bytes, 1);
  appendString(bytes, "Sách cũ");
  appendString(bytes, "Tác giả");
  appendString(bytes, bookPath);
  appendU16(bytes, clipping.spineIndex);
  appendU16(bytes, clipping.startPage);
  appendU16(bytes, clipping.endPage);
  appendU16(bytes, clipping.pageCount);
  appendU16(bytes, clipping.startWordIndex);
  appendU16(bytes, clipping.endWordIndex);
  appendU16(bytes, clipping.wordCount);
  appendU16(bytes, clipping.paragraphIndex);
  appendU32(bytes, clipping.timestamp);
  std::array<uint8_t, 48> chapter{};
  std::copy(clipping.chapterTitle.begin(), clipping.chapterTitle.end(), chapter.begin());
  bytes.insert(bytes.end(), chapter.begin(), chapter.end());
  if (version == 1) {
    appendU32(bytes, static_cast<uint32_t>(text.size()));
  } else {
    appendU16(bytes, static_cast<uint16_t>(text.size()));
  }
  bytes.insert(bytes.end(), text.begin(), text.end());
  return bytes;
}

bool memoryReadAt(void* context, const uint32_t offset, uint8_t* data, const size_t length) {
  const auto& bytes = *static_cast<std::vector<uint8_t>*>(context);
  if (offset > bytes.size() || length > bytes.size() - offset) return false;
  std::copy_n(bytes.data() + offset, length, data);
  return true;
}

ClippingCodec::Status inspectBytes(const std::vector<uint8_t>& bytes, ClippingCodec::Index& index) {
  return ClippingCodec::inspect(
      {const_cast<std::vector<uint8_t>*>(&bytes), static_cast<uint32_t>(bytes.size()), memoryReadAt}, index);
}

constexpr char BOOK_PATH[] = "/Books/demo.epub";
constexpr char MOVED_BOOK_PATH[] = "/read/demo.epub";

class ClippingStorePersistenceTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }

  std::string storePath() const { return ClippingCodec::filePathForBook(BOOK_PATH); }
  std::string movedStorePath() const { return ClippingCodec::filePathForBook(MOVED_BOOK_PATH); }

  std::vector<uint8_t> createCanonicalFor(const std::string& bookPath, const std::string& text = "nội dung đầu") {
    ClippingStore store;
    EXPECT_EQ(store.loadForBook(bookPath, "Sách", "Tác giả"), ClippingStore::LoadResult::Ready);
    EXPECT_EQ(store.add(sampleClipping(), text), ClippingStore::AddResult::Added);
    return Storage.file(ClippingCodec::filePathForBook(bookPath));
  }

  std::vector<uint8_t> createCanonical() { return createCanonicalFor(BOOK_PATH); }
};

}  // namespace

TEST_F(ClippingStorePersistenceTest, RoundTripsMetadataAndLoadsTextOnlyWhenRequested) {
  createCanonical();
  ClippingStore loaded;
  ASSERT_EQ(loaded.loadForBook(BOOK_PATH, "Tên mới", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(loaded.size(), 1U);
  EXPECT_EQ(loaded.at(0)->chapterTitle, "Chương thử");
  std::string text;
  EXPECT_TRUE(loaded.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");
}

TEST_F(ClippingStorePersistenceTest, LazyTextReadRevalidatesTheStoredPayloadChecksum) {
  const auto original = createCanonical();
  ClippingStore loaded;
  ASSERT_EQ(loaded.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);

  auto changed = original;
  changed.back() ^= 0x01;
  Storage.setFile(storePath(), std::move(changed));

  std::string text = "stale";
  EXPECT_FALSE(loaded.readText(0, text));
  EXPECT_TRUE(text.empty());

  Storage.setFile(storePath(), original);
  EXPECT_TRUE(loaded.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");
}

TEST_F(ClippingStorePersistenceTest, ShortWriteAndSyncFailureLeaveCanonicalUntouched) {
  const auto original = createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);

  Storage.shortWriteOnce();
  EXPECT_EQ(store.add(sampleClipping(43), "không được lưu"), ClippingStore::AddResult::SaveFailed);
  EXPECT_EQ(Storage.file(storePath()), original);
  EXPECT_EQ(store.size(), 1U);

  Storage.failSyncOnce();
  EXPECT_EQ(store.add(sampleClipping(44), "cũng không lưu"), ClippingStore::AddResult::SaveFailed);
  EXPECT_EQ(Storage.file(storePath()), original);
  EXPECT_EQ(store.size(), 1U);
}

TEST_F(ClippingStorePersistenceTest, FailedPromotionAndRollbackLeaveRecoverableFilesAndFailClosed) {
  const auto original = createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);

  // canonical -> backup succeeds; temp -> canonical and backup -> canonical fail.
  Storage.failRenameOnCalls({2, 3});
  EXPECT_EQ(store.add(sampleClipping(43), "đoạn chưa commit"), ClippingStore::AddResult::SaveFailed);
  EXPECT_FALSE(store.isLoaded());
  EXPECT_EQ(store.size(), 0U);
  EXPECT_FALSE(Storage.exists(storePath().c_str()));
  EXPECT_EQ(Storage.file(storePath() + ".bak"), original);
  EXPECT_TRUE(Storage.exists((storePath() + ".tmp").c_str()));

  ClippingStore recovered;
  ASSERT_EQ(recovered.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Recovered);
  ASSERT_EQ(recovered.size(), 1U);
  std::string text;
  EXPECT_TRUE(recovered.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");
}

TEST_F(ClippingStorePersistenceTest, RecoversValidatedBackupBeforeUsingCorruptCanonical) {
  const auto original = createCanonical();
  Storage.setFile(storePath() + ".bak", original);
  auto corrupt = original;
  corrupt.pop_back();
  Storage.setFile(storePath(), std::move(corrupt));

  ClippingStore recovered;
  ASSERT_EQ(recovered.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Recovered);
  EXPECT_EQ(Storage.file(storePath()), original);
  std::string text;
  EXPECT_TRUE(recovered.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");
}

TEST_F(ClippingStorePersistenceTest, NeverReplacesAnUninspectableCanonicalWithAnOlderBackup) {
  const auto validBackup = createCanonical();

  const std::vector<uint8_t> oversized(ClippingCodec::MAX_FILE_BYTES + 1, 0xA5);
  Storage.setFile(storePath(), oversized);
  Storage.setFile(storePath() + ".bak", validBackup);
  ClippingStore oversizedStore;
  EXPECT_EQ(oversizedStore.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::InvalidFile);
  EXPECT_EQ(Storage.file(storePath()), oversized);
  EXPECT_EQ(Storage.file(storePath() + ".bak"), validBackup);

  Storage.setFile(storePath(), validBackup);
  Storage.makeUnreadable(storePath());
  ClippingStore unreadableStore;
  EXPECT_EQ(unreadableStore.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::IoError);
  EXPECT_EQ(Storage.file(storePath()), validBackup);
  EXPECT_EQ(Storage.file(storePath() + ".bak"), validBackup);
}

TEST_F(ClippingStorePersistenceTest, RecoversValidatedTempWhenNoCommittedCopyExists) {
  const auto original = createCanonical();
  ASSERT_TRUE(Storage.remove(storePath().c_str()));
  Storage.setFile(storePath() + ".tmp", original);

  ClippingStore recovered;
  ASSERT_EQ(recovered.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Recovered);
  EXPECT_EQ(Storage.file(storePath()), original);
}

TEST_F(ClippingStorePersistenceTest, NeverPromotesTempPastAnUninspectableCommittedBackup) {
  const auto original = createCanonical();
  ASSERT_TRUE(Storage.remove(storePath().c_str()));
  Storage.setFile(storePath() + ".bak", original);
  Storage.setFile(storePath() + ".tmp", original);
  Storage.makeUnreadable(storePath() + ".bak");

  ClippingStore rejected;
  EXPECT_EQ(rejected.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::IoError);
  EXPECT_FALSE(Storage.exists(storePath().c_str()));
  EXPECT_EQ(Storage.file(storePath() + ".bak"), original);
  EXPECT_EQ(Storage.file(storePath() + ".tmp"), original);
}

TEST_F(ClippingStorePersistenceTest, NeverOverwritesCanonicalOrSiblingFromANewerVersion) {
  auto newer = createCanonical();
  newer[4] = static_cast<uint8_t>(ClippingCodec::VERSION + 1);
  newer[5] = 0;
  Storage.setFile(storePath(), newer);

  ClippingStore rejected;
  EXPECT_EQ(rejected.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::UnsupportedVersion);
  EXPECT_EQ(Storage.file(storePath()), newer);

  Storage.reset();
  const auto original = createCanonical();
  ClippingStore loaded;
  ASSERT_EQ(loaded.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  Storage.setFile(storePath() + ".bak", newer);
  EXPECT_EQ(loaded.add(sampleClipping(45), "bị chặn"), ClippingStore::AddResult::SaveFailed);
  EXPECT_EQ(Storage.file(storePath()), original);
  EXPECT_EQ(Storage.file(storePath() + ".bak"), newer);
}

TEST_F(ClippingStorePersistenceTest, NeverDeletesAnOversizedSiblingThatMayBelongToANewerFormat) {
  const auto original = createCanonical();
  ClippingStore loaded;
  ASSERT_EQ(loaded.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  const std::vector<uint8_t> oversized(ClippingCodec::MAX_FILE_BYTES + 1, 0xA5);
  Storage.setFile(storePath() + ".bak", oversized);

  EXPECT_EQ(loaded.add(sampleClipping(45), "bị chặn"), ClippingStore::AddResult::SaveFailed);
  EXPECT_EQ(Storage.file(storePath()), original);
  EXPECT_EQ(Storage.file(storePath() + ".bak"), oversized);
}

TEST_F(ClippingStorePersistenceTest, MigratesOnlyFullyValidatedCrossInkV1AndV2) {
  for (const uint8_t version : {uint8_t{1}, uint8_t{2}}) {
    Storage.reset();
    const auto legacy = makeLegacy(version, BOOK_PATH, "văn bản cũ");
    Storage.setFile(storePath(), legacy);

    ClippingStore migrated;
    ASSERT_EQ(migrated.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Migrated);
    ClippingCodec::Index index;
    ASSERT_EQ(inspectBytes(Storage.file(storePath()), index), ClippingCodec::Status::Ok);
    EXPECT_EQ(index.format, ClippingCodec::Format::Current);
    std::string text;
    EXPECT_TRUE(migrated.readText(0, text));
    EXPECT_EQ(text, "văn bản cũ");

    Storage.reset();
    auto ambiguous = legacy;
    ambiguous.push_back(0);
    Storage.setFile(storePath(), ambiguous);
    ClippingStore rejected;
    EXPECT_EQ(rejected.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::InvalidFile);
    EXPECT_EQ(Storage.file(storePath()), ambiguous);
  }
}

TEST_F(ClippingStorePersistenceTest, RekeysByStreamingToTheMovedBookAndSwitchesOnlyAfterVerification) {
  createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(store.add(sampleClipping(43), "đoạn thứ hai"), ClippingStore::AddResult::Added);

  ASSERT_EQ(store.rekeyForBook(MOVED_BOOK_PATH, "Sách đã chuyển", "Tác giả mới"), ClippingStore::RekeyResult::Rekeyed);
  EXPECT_FALSE(Storage.exists(storePath().c_str()));
  EXPECT_TRUE(Storage.exists(movedStorePath().c_str()));
  EXPECT_FALSE(Storage.exists((movedStorePath() + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((movedStorePath() + ".bak").c_str()));
  EXPECT_EQ(store.path(), movedStorePath());
  EXPECT_EQ(store.book().path, MOVED_BOOK_PATH);
  EXPECT_EQ(store.book().title, "Sách đã chuyển");
  EXPECT_EQ(store.book().author, "Tác giả mới");

  ClippingCodec::Index migrated;
  ASSERT_EQ(inspectBytes(Storage.file(movedStorePath()), migrated), ClippingCodec::Status::Ok);
  EXPECT_EQ(migrated.format, ClippingCodec::Format::Current);
  EXPECT_EQ(migrated.book.path, MOVED_BOOK_PATH);
  EXPECT_EQ(migrated.book.title, "Sách đã chuyển");
  EXPECT_EQ(migrated.clippings.size(), 2U);
  std::string text;
  ASSERT_TRUE(store.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");
  ASSERT_TRUE(store.readText(1, text));
  EXPECT_EQ(text, "đoạn thứ hai");
}

TEST_F(ClippingStorePersistenceTest, TwoPhaseRekeyKeepsSourceUntilTheBookMoveIsDurable) {
  const auto original = createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);

  ASSERT_EQ(store.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách đã chuyển", "Tác giả"),
            ClippingStore::RekeyResult::Prepared);
  EXPECT_TRUE(store.hasPreparedRekey());
  EXPECT_EQ(store.path(), storePath());
  EXPECT_EQ(Storage.file(storePath()), original);
  EXPECT_TRUE(Storage.exists(movedStorePath().c_str()));

  // A reset before the EPUB rename still opens the old key.
  ClippingStore beforeBookRenameCrash;
  ASSERT_EQ(beforeBookRenameCrash.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  std::string text;
  ASSERT_TRUE(beforeBookRenameCrash.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");

  // A reset after the EPUB rename opens the already durable destination even
  // if finalize was not reached.
  ClippingStore afterBookRenameCrash;
  ASSERT_EQ(afterBookRenameCrash.loadForBook(MOVED_BOOK_PATH, "Sách đã chuyển", "Tác giả"),
            ClippingStore::LoadResult::Loaded);
  ASSERT_TRUE(afterBookRenameCrash.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");

  ASSERT_EQ(store.finalizePreparedRekey(), ClippingStore::RekeyResult::Rekeyed);
  EXPECT_FALSE(store.hasPreparedRekey());
  EXPECT_EQ(store.path(), movedStorePath());
  EXPECT_FALSE(Storage.exists(storePath().c_str()));
  EXPECT_TRUE(store.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");
}

TEST_F(ClippingStorePersistenceTest, ReplacementRemovesOnlyItsExactlyOwnedMoveMarker) {
  createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(store.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách đã chuyển", "Tác giả"),
            ClippingStore::RekeyResult::Prepared);
  const std::string marker = movedStorePath() + ".move";
  ASSERT_TRUE(Storage.exists(marker.c_str()));

  EXPECT_TRUE(ClippingStore::removeFilesForBook(MOVED_BOOK_PATH));

  EXPECT_FALSE(Storage.exists(movedStorePath().c_str()));
  EXPECT_FALSE(Storage.exists(marker.c_str()));
  EXPECT_TRUE(Storage.exists(storePath().c_str()));
}

TEST_F(ClippingStorePersistenceTest, ReplacementPreservesMalformedMoveMarker) {
  createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(store.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách đã chuyển", "Tác giả"),
            ClippingStore::RekeyResult::Prepared);
  const std::string marker = movedStorePath() + ".move";
  const std::vector<uint8_t> malformed{0x01, 0x02, 0x03};
  Storage.setFile(marker, malformed);

  EXPECT_FALSE(ClippingStore::removeFilesForBook(MOVED_BOOK_PATH));

  EXPECT_EQ(Storage.file(marker), malformed);
  EXPECT_TRUE(Storage.exists(storePath().c_str()));
}

TEST_F(ClippingStorePersistenceTest, ReplacementPreservesMismatchedMoveMarker) {
  createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(store.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách đã chuyển", "Tác giả"),
            ClippingStore::RekeyResult::Prepared);
  const std::string marker = movedStorePath() + ".move";
  auto mismatched = Storage.file(marker);
  const auto destination =
      std::search(mismatched.begin(), mismatched.end(), MOVED_BOOK_PATH, MOVED_BOOK_PATH + sizeof(MOVED_BOOK_PATH) - 1);
  ASSERT_NE(destination, mismatched.end());
  *destination = '?';
  Storage.setFile(marker, mismatched);

  EXPECT_FALSE(ClippingStore::removeFilesForBook(MOVED_BOOK_PATH));

  EXPECT_EQ(Storage.file(marker), mismatched);
  EXPECT_TRUE(Storage.exists(storePath().c_str()));
}

TEST_F(ClippingStorePersistenceTest, PreparedRekeyCanBeCancelledOrReusedAfterAReset) {
  createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(store.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::Prepared);
  EXPECT_TRUE(store.cancelPreparedRekey());
  EXPECT_FALSE(Storage.exists(movedStorePath().c_str()));
  EXPECT_TRUE(Storage.exists(storePath().c_str()));

  ASSERT_EQ(store.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::Prepared);
  ClippingStore retryAfterReset;
  ASSERT_EQ(retryAfterReset.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  EXPECT_EQ(retryAfterReset.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"),
            ClippingStore::RekeyResult::Prepared);
  EXPECT_EQ(retryAfterReset.finalizePreparedRekey(), ClippingStore::RekeyResult::Rekeyed);
  EXPECT_FALSE(Storage.exists(storePath().c_str()));
  EXPECT_TRUE(Storage.exists(movedStorePath().c_str()));
}

TEST_F(ClippingStorePersistenceTest, InterruptedPreparedCopyIsRebuiltFromChangedAuthoritativeSource) {
  createCanonical();
  Storage.setFile(BOOK_PATH, {0x01});
  ClippingStore first;
  ASSERT_EQ(first.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(first.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::Prepared);

  ClippingStore changed;
  ASSERT_EQ(changed.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(changed.add(sampleClipping(99), "đoạn mới sau reset"), ClippingStore::AddResult::Added);

  ClippingStore retry;
  ASSERT_EQ(retry.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(retry.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::Prepared);
  ClippingStore destination;
  ASSERT_EQ(destination.loadForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  EXPECT_EQ(destination.size(), 2U);
}

TEST_F(ClippingStorePersistenceTest, MarkerOwnedPartialDestinationIsCleanedAndRebuilt) {
  createCanonical();
  Storage.setFile(BOOK_PATH, {0x01});
  ClippingStore first;
  ASSERT_EQ(first.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(first.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::Prepared);
  ASSERT_TRUE(Storage.remove(movedStorePath().c_str()));
  Storage.setFile(movedStorePath() + ".tmp", {0x01, 0x02, 0x03});

  ClippingStore retry;
  ASSERT_EQ(retry.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  EXPECT_EQ(retry.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::Prepared);
  EXPECT_TRUE(Storage.exists(movedStorePath().c_str()));
  EXPECT_FALSE(Storage.exists((movedStorePath() + ".tmp").c_str()));
}

TEST_F(ClippingStorePersistenceTest, RetryPromotesOnlyAnExactValidatedPreparedTemp) {
  createCanonical();
  ClippingStore firstAttempt;
  ASSERT_EQ(firstAttempt.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(firstAttempt.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::Prepared);
  const auto prepared = Storage.file(movedStorePath());
  ASSERT_TRUE(firstAttempt.cancelPreparedRekey());
  Storage.setFile(movedStorePath() + ".tmp", prepared);

  ClippingStore retry;
  ASSERT_EQ(retry.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  EXPECT_EQ(retry.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::Prepared);
  EXPECT_FALSE(Storage.exists((movedStorePath() + ".tmp").c_str()));
  EXPECT_EQ(Storage.file(movedStorePath()), prepared);

  ASSERT_TRUE(retry.cancelPreparedRekey());
  auto altered = prepared;
  altered.back() ^= 1;
  Storage.setFile(movedStorePath() + ".tmp", std::move(altered));
  EXPECT_EQ(retry.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"),
            ClippingStore::RekeyResult::DestinationExists);
  EXPECT_TRUE(Storage.exists((movedStorePath() + ".tmp").c_str()));
}

TEST_F(ClippingStorePersistenceTest, RekeyNeverOverwritesDestinationOrNewerDestinationSibling) {
  const auto original = createCanonical();
  ClippingStore source;
  ASSERT_EQ(source.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);

  const auto existingDestination = createCanonicalFor(MOVED_BOOK_PATH, "đích đã tồn tại");
  EXPECT_EQ(source.rekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::DestinationExists);
  EXPECT_EQ(Storage.file(storePath()), original);
  EXPECT_EQ(Storage.file(movedStorePath()), existingDestination);

  for (const std::string& suffix : {std::string{}, std::string{".tmp"}, std::string{".bak"}}) {
    SCOPED_TRACE(suffix);
    Storage.reset();
    const auto sourceBytes = createCanonical();
    ClippingStore loaded;
    ASSERT_EQ(loaded.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
    auto newer = sourceBytes;
    newer[4] = static_cast<uint8_t>(ClippingCodec::VERSION + 1);
    newer[5] = 0;
    Storage.setFile(movedStorePath() + suffix, newer);

    EXPECT_EQ(loaded.rekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::UnsupportedVersion);
    EXPECT_EQ(Storage.file(storePath()), sourceBytes);
    EXPECT_EQ(Storage.file(movedStorePath() + suffix), newer);
  }
}

TEST_F(ClippingStorePersistenceTest, RekeyFailuresKeepOldCanonicalAndRollBackDestination) {
  enum class Failure { ShortWrite, Sync, Rename, FinalCorruption };
  for (const Failure failure : {Failure::ShortWrite, Failure::Sync, Failure::Rename, Failure::FinalCorruption}) {
    SCOPED_TRACE(static_cast<int>(failure));
    Storage.reset();
    const auto original = createCanonical();
    ClippingStore store;
    ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);

    switch (failure) {
      case Failure::ShortWrite:
        Storage.shortWriteOnce();
        break;
      case Failure::Sync:
        Storage.failSyncOnce();
        break;
      case Failure::Rename:
        Storage.failRenameOnce();
        break;
      case Failure::FinalCorruption:
        Storage.corruptRenameOnce();
        break;
    }

    EXPECT_EQ(store.rekeyForBook(MOVED_BOOK_PATH, "Sách mới", "Tác giả"), ClippingStore::RekeyResult::IoError);
    EXPECT_EQ(Storage.file(storePath()), original);
    EXPECT_FALSE(Storage.exists(movedStorePath().c_str()));
    EXPECT_FALSE(Storage.exists((movedStorePath() + ".tmp").c_str()));
    EXPECT_EQ(store.path(), storePath());
    std::string text;
    EXPECT_TRUE(store.readText(0, text));
    EXPECT_EQ(text, "nội dung đầu");
  }
}

TEST_F(ClippingStorePersistenceTest, FinalizedRekeyKeepsTheVerifiedDestinationWhenSourceCleanupFails) {
  const auto original = createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  ASSERT_EQ(store.prepareRekeyForBook(MOVED_BOOK_PATH, "Sách mới", "Tác giả"), ClippingStore::RekeyResult::Prepared);

  Storage.failRemoveOnce();
  EXPECT_EQ(store.finalizePreparedRekey(), ClippingStore::RekeyResult::Rekeyed);
  EXPECT_EQ(store.path(), movedStorePath());
  EXPECT_EQ(Storage.file(storePath()), original);
  EXPECT_TRUE(Storage.exists(movedStorePath().c_str()));
  std::string text;
  EXPECT_TRUE(store.readText(0, text));
  EXPECT_EQ(text, "nội dung đầu");
}

TEST_F(ClippingStorePersistenceTest, RekeyRejectsChangedSourceWithoutTouchingEitherKey) {
  createCanonical();
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Loaded);
  auto changed = Storage.file(storePath());
  changed.pop_back();
  Storage.setFile(storePath(), changed);

  EXPECT_EQ(store.rekeyForBook(MOVED_BOOK_PATH, "Sách", "Tác giả"), ClippingStore::RekeyResult::SourceInvalid);
  EXPECT_EQ(Storage.file(storePath()), changed);
  EXPECT_FALSE(Storage.exists(movedStorePath().c_str()));
  EXPECT_FALSE(Storage.exists((movedStorePath() + ".tmp").c_str()));
  EXPECT_EQ(store.path(), storePath());
}

TEST_F(ClippingStorePersistenceTest, RekeyOfFreshEmptyStoreChangesKeyWithoutCreatingAFile) {
  ClippingStore store;
  ASSERT_EQ(store.loadForBook(BOOK_PATH, "Sách", "Tác giả"), ClippingStore::LoadResult::Ready);
  ASSERT_EQ(store.rekeyForBook(MOVED_BOOK_PATH, "Sách mới", "Tác giả"), ClippingStore::RekeyResult::Rekeyed);
  EXPECT_EQ(store.path(), movedStorePath());
  EXPECT_FALSE(Storage.exists(storePath().c_str()));
  EXPECT_FALSE(Storage.exists(movedStorePath().c_str()));

  EXPECT_EQ(store.add(sampleClipping(), "ghi theo khóa mới"), ClippingStore::AddResult::Added);
  EXPECT_FALSE(Storage.exists(storePath().c_str()));
  EXPECT_TRUE(Storage.exists(movedStorePath().c_str()));
}

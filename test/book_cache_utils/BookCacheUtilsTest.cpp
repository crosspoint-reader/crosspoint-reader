#include <Epub.h>
#include <HalStorage.h>
#include <Txt.h>
#include <Xtc.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "BookCacheUtils.h"
#include "BookReplacementTransaction.h"
#include "BookmarkUtil.h"

namespace {

constexpr char SOURCE_BOOK_PATH[] = "/books/source.epub";
constexpr char DESTINATION_BOOK_PATH[] = "/read/source.epub";
const std::string CACHE_PATH = Epub(SOURCE_BOOK_PATH, "/.crosspoint").getCachePath();
const std::string MOVED_CACHE_PATH = Epub(DESTINATION_BOOK_PATH, "/.crosspoint").getCachePath();

std::string siblingPathWithPrefix(const std::string& path, const std::string& prefix) {
  const size_t separator = path.rfind('/');
  return path.substr(0, separator + 1) + prefix + path.substr(separator + 1);
}

const std::string STAGING_PATH = siblingPathWithPrefix(CACHE_PATH, ".crossvi_clear_");
const std::string MOVE_STAGING_PATH = siblingPathWithPrefix(MOVED_CACHE_PATH, ".crossvi_move_");
const std::string MOVE_CLEANUP_PATH = siblingPathWithPrefix(MOVED_CACHE_PATH, ".crossvi_cleanup_");
const std::string DISCARD_PATH = siblingPathWithPrefix(CACHE_PATH, ".crossvi_discard_");

std::string discardPathFor(const unsigned slot) {
  return slot == 0 ? DISCARD_PATH : DISCARD_PATH + "_" + std::to_string(slot + 1);
}

std::vector<unsigned char> bytes(const unsigned char value) { return {value, static_cast<unsigned char>(value + 1)}; }

std::vector<unsigned char> emptyBookmarks() {
  constexpr char EMPTY[] = "{\"bookmarks\":[]}";
  return std::vector<unsigned char>(EMPTY, EMPTY + sizeof(EMPTY) - 1);
}

std::vector<unsigned char> moveMarker(const std::string& sourceBookPath, const std::string& destinationBookPath,
                                      const std::string& sourceCachePath, const std::string& destinationCachePath) {
  const std::array<std::string, 4> paths = {sourceBookPath, destinationBookPath, sourceCachePath, destinationCachePath};
  std::vector<unsigned char> marker = {'C', 'V', 'M', 'S', '2'};
  for (const std::string& path : paths) {
    const auto length = static_cast<uint16_t>(path.size());
    marker.push_back(static_cast<unsigned char>(length & 0xFFU));
    marker.push_back(static_cast<unsigned char>((length >> 8U) & 0xFFU));
  }
  for (const std::string& path : paths) marker.insert(marker.end(), path.begin(), path.end());
  return marker;
}

class BookCacheUtilsTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.addDirectory(CACHE_PATH);
    Storage.addDirectory("/read");
    Storage.addDirectory(BookmarkUtil::getBookmarksDir());
    Storage.setFile(SOURCE_BOOK_PATH, bytes(240));
  }

  void put(const std::string& name, const unsigned char value) {
    Storage.setFile(std::string(CACHE_PATH) + "/" + name, bytes(value));
  }

  void expectPreserved(const std::map<std::string, std::vector<unsigned char>>& expected) {
    for (const auto& [name, content] : expected) {
      const std::string path = std::string(CACHE_PATH) + "/" + name;
      ASSERT_TRUE(Storage.exists(path.c_str())) << name;
      EXPECT_EQ(Storage.file(path), content) << name;
    }
  }
};

TEST_F(BookCacheUtilsTest, ClearsDerivedCacheAndPreservesAllSupportedUserState) {
  const std::vector<std::string> names = {"stats.bin",
                                          "stats_v5.bin",
                                          "stats_v5.bin.bak",
                                          "stats_v5.bin.tmp",
                                          "stats_v6.bin",
                                          "stats_v6.bin.bak",
                                          "stats_v6.bin.tmp",
                                          "progress.bin",
                                          "progress.bin.bak",
                                          "progress.bin.tmp",
                                          "reader_settings.bin",
                                          "reader_settings.bin.bak",
                                          "reader_settings.bin.tmp",
                                          "crossvi_reader_settings.bin",
                                          "crossvi_reader_settings.bin.bak",
                                          "crossvi_reader_settings.bin.tmp",
                                          "source_identity.bin",
                                          "source_identity.bin.bak",
                                          "source_identity.bin.tmp",
                                          ".crossvi_replaced_clippings.bin",
                                          ".crossvi_replaced_clippings.bin.bak",
                                          ".crossvi_replaced_clippings.bin.tmp",
                                          ".crossvi_replaced_clippings.move",
                                          ".crossvi_replaced_bookmark.json",
                                          ".crossvi_replaced_bookmark.json.2",
                                          ".crossvi_replaced_bookmark.json.4",
                                          ".crossvi_replaced_bookmark.json.6",
                                          ".crossvi_replaced_bookmark.json.65535"};
  std::map<std::string, std::vector<unsigned char>> expected;
  unsigned char value = 1;
  for (const std::string& name : names) {
    put(name, value);
    expected.emplace(name, bytes(value++));
  }
  put("index.bin", 90);
  Storage.addDirectory(std::string(CACHE_PATH) + "/sections");
  Storage.setFile(std::string(CACHE_PATH) + "/sections/1.bin", bytes(92));

  ASSERT_TRUE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  expectPreserved(expected);
  EXPECT_EQ(Storage.filesUnder(CACHE_PATH).size(), expected.size());
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/index.bin").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/sections").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, WhitelistStagesOnlyExactNamesAndPreservesUnknownLookalikes) {
  const std::vector<std::string> preserved = {"stats.bin",
                                              "stats_v0.bin",
                                              "stats_v0000000000001.bin.tmp",
                                              "stats_v42.bin.bak",
                                              "progress.bin",
                                              "progress.bin.bak",
                                              "progress.bin.tmp",
                                              "reader_settings.bin",
                                              "reader_settings.bin.bak",
                                              "reader_settings.bin.tmp",
                                              "crossvi_reader_settings.bin",
                                              "crossvi_reader_settings.bin.bak",
                                              "crossvi_reader_settings.bin.tmp",
                                              "source_identity.bin",
                                              "source_identity.bin.bak",
                                              "source_identity.bin.tmp"};
  const std::vector<std::string> unknown = {"stats.bin.bak",
                                            "stats.bin.tmp",
                                            "stats_v.bin",
                                            "stats_vx.bin",
                                            "stats_v1x.bin",
                                            "stats_v5.bin.old",
                                            "stats_v5.bin.bak.old",
                                            "stats_v-1.bin",
                                            "Stats_v5.bin",
                                            "mystats_v5.bin",
                                            "progress.bin.old",
                                            "reader_settings.bin.old",
                                            "crossvi_reader_settings.bin.old",
                                            "source_identity.bin.old"};

  unsigned char value = 1;
  for (const std::string& name : preserved) put(name, value++);
  for (const std::string& name : unknown) put(name, value++);
  put("book.bin", value++);

  ASSERT_TRUE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  for (const std::string& name : preserved) {
    EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/" + name).c_str())) << name;
  }
  for (const std::string& name : unknown) {
    EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/" + name).c_str())) << name;
  }
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/book.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, ClearPreservesUnknownFilesAndDirectories) {
  put("stats_v5.bin", 1);
  put("index.bin", 2);
  put("future_state_v9.bin", 3);
  Storage.addDirectory(std::string(CACHE_PATH) + "/future_format");
  Storage.setFile(std::string(CACHE_PATH) + "/future_format/payload.bin", bytes(4));

  ASSERT_TRUE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), bytes(1));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/index.bin").c_str()));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/future_state_v9.bin"), bytes(3));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/future_format/payload.bin"), bytes(4));
}

TEST_F(BookCacheUtilsTest, StageFailureRollsBackWithoutClearingAnything) {
  put("crossvi_reader_settings.bin", 1);
  put("stats_v5.bin", 2);
  put("index.bin", 3);
  const auto settings = Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin");
  const auto stats = Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin");
  Storage.failRenameOnCall(2);

  EXPECT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin"), settings);
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/index.bin").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, ClearFailureRestoresStateAndLeavesDerivedEntryForRetry) {
  put("stats_v5.bin", 1);
  put("index.bin", 2);
  const auto stats = Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin");
  Storage.failDeletePathOnce(std::string(CACHE_PATH) + "/index.bin");

  EXPECT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/index.bin").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, InterruptedRestoreIsRecoveredOnNextCall) {
  put("crossvi_reader_settings.bin", 1);
  put("stats_v5.bin", 2);
  put("index.bin", 3);
  const auto settings = Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin");
  const auto stats = Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin");

  // Two moves into staging, then fail the first restore.
  Storage.failRenameOnCall(3);
  EXPECT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));
  EXPECT_TRUE(Storage.exists(STAGING_PATH));

  ASSERT_TRUE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin"), settings);
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/index.bin").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, ReaderRecoveryFinishesAnIncompleteClearRollbackWithoutClearingAgain) {
  put("crossvi_reader_settings.bin", 1);
  put("stats_v5.bin", 2);
  put("index.bin", 3);
  const auto settings = Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin");
  const auto stats = Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin");

  // Two moves into staging, then fail the first rollback move. This mirrors
  // the reader-menu failure path before it decides whether it may keep running.
  Storage.failRenameOnCall(3);
  ASSERT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));
  ASSERT_TRUE(Storage.exists(STAGING_PATH));

  ASSERT_TRUE(recoverBookCacheUserState(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin"), settings);
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, OpenTimeRecoveryRestoresStateWithoutClearingDerivedCache) {
  const auto settings = bytes(1);
  const auto stats = bytes(2);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/crossvi_reader_settings.bin", settings);
  Storage.setFile(std::string(STAGING_PATH) + "/stats_v5.bin", stats);
  put("index.bin", 3);

  ASSERT_TRUE(recoverBookCacheUserState(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/crossvi_reader_settings.bin"), settings);
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), stats);
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/index.bin").c_str()));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, OpenTimeRecoveryFailsClosedOnConflictingState) {
  put("stats_v5.bin", 1);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/stats_v5.bin", bytes(2));

  EXPECT_FALSE(recoverBookCacheUserState(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), bytes(1));
  EXPECT_EQ(Storage.file(std::string(STAGING_PATH) + "/stats_v5.bin"), bytes(2));
}

TEST_F(BookCacheUtilsTest, UnexpectedStagingContentIsNeverDeleted) {
  put("stats_v5.bin", 1);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/not-user-state.bin", bytes(9));

  EXPECT_FALSE(clearBookCacheDirectoryPreservingUserState(CACHE_PATH));

  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/stats_v5.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(STAGING_PATH) + "/not-user-state.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, ReplacementDiscardsVerifiedClearStagingBeforeItCanRevive) {
  put("stats_v5.bin", 1);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/crossvi_reader_settings.bin", bytes(2));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(STAGING_PATH));
  ASSERT_TRUE(recoverBookCacheUserState(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/stats_v5.bin").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/crossvi_reader_settings.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, ReplacementQuarantinesUserStateWithoutDeletingIt) {
  put("progress.bin", 1);
  put("crossvi_reader_settings.bin", 2);
  put("source_identity.bin", 3);

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/progress.bin"), bytes(1));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/crossvi_reader_settings.bin"), bytes(2));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/source_identity.bin"), bytes(3));
}

TEST_F(BookCacheUtilsTest, ReplacementArchivePurgesOnlyKnownDerivedCacheEntries) {
  put("progress.bin", 1);
  put("reader_settings.bin", 2);
  put("book.bin", 3);
  put("index.bin", 4);
  put("css_rules.cache", 5);
  put("cover_crop.bmp", 6);
  put("thumb_120.bmp", 7);
  put("img_2_7.JPG", 8);
  put("thumb_notes.bmp", 9);
  put("img_notes.jpg", 10);
  put("future_migration.bin", 11);
  Storage.addDirectory(std::string(CACHE_PATH) + "/sections");
  Storage.setFile(std::string(CACHE_PATH) + "/sections/0.bin", bytes(12));
  Storage.addDirectory(std::string(CACHE_PATH) + "/html");
  Storage.setFile(std::string(CACHE_PATH) + "/html/0.html", bytes(13));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_EQ(Storage.file(DISCARD_PATH + "/progress.bin"), bytes(1));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/reader_settings.bin"), bytes(2));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/thumb_notes.bmp"), bytes(9));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/img_notes.jpg"), bytes(10));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/future_migration.bin"), bytes(11));
  for (const char* name : {"book.bin", "index.bin", "css_rules.cache", "cover_crop.bmp", "thumb_120.bmp", "img_2_7.JPG",
                           "sections", "html"}) {
    EXPECT_FALSE(Storage.exists(DISCARD_PATH + "/" + name)) << name;
  }
}

TEST_F(BookCacheUtilsTest, DerivedArchivePurgeFailureDoesNotUndoTheQuarantineCommit) {
  put("progress.bin", 1);
  put("book.bin", 2);
  Storage.addDirectory(std::string(CACHE_PATH) + "/sections");
  Storage.setFile(std::string(CACHE_PATH) + "/sections/0.bin", bytes(3));
  Storage.failDeletePathOnce(DISCARD_PATH + "/book.bin");

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/progress.bin"), bytes(1));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/book.bin"), bytes(2));
  EXPECT_FALSE(Storage.exists(DISCARD_PATH + "/sections"));
}

TEST_F(BookCacheUtilsTest, ReplacementRenamesCanonicalCacheAtTheFinalMutationBoundary) {
  put("progress.bin", 1);
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/crossvi_reader_settings.bin", bytes(2));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  ASSERT_FALSE(Storage.renameHistory().empty());
  EXPECT_EQ(Storage.renameHistory().back().first, CACHE_PATH);
  EXPECT_EQ(Storage.renameHistory().back().second, DISCARD_PATH);
}

TEST_F(BookCacheUtilsTest, ReplacementPolicyNoOpsForNewOrByteIdenticalBooks) {
  for (const auto disposition : {BookReplacementDisposition::NoState, BookReplacementDisposition::SameSource}) {
    int calls = 0;
    EXPECT_TRUE(runBookReplacementTransaction(
        disposition,
        [&] {
          ++calls;
          return true;
        },
        [&] {
          ++calls;
          return true;
        },
        [&] {
          ++calls;
          return true;
        }));
    EXPECT_EQ(calls, 0);
  }
}

TEST_F(BookCacheUtilsTest, ReplacementPolicyKeepsCacheCommitLastAcrossFaultAndRetry) {
  for (int failedStep = 0; failedStep < 3; ++failedStep) {
    SCOPED_TRACE(failedStep);
    std::vector<int> calls;
    const auto step = [&](const int index) {
      calls.push_back(index);
      return index != failedStep;
    };
    EXPECT_FALSE(runBookReplacementTransaction(
        BookReplacementDisposition::Quarantine, [&] { return step(0); }, [&] { return step(1); },
        [&] { return step(2); }));
    EXPECT_EQ(calls.back(), failedStep);
    EXPECT_EQ(std::count(calls.begin(), calls.end(), 2), failedStep == 2 ? 1 : 0);

    calls.clear();
    EXPECT_TRUE(runBookReplacementTransaction(
        BookReplacementDisposition::Quarantine,
        [&] {
          calls.push_back(0);
          return true;
        },
        [&] {
          calls.push_back(1);
          return true;
        },
        [&] {
          calls.push_back(2);
          return true;
        }));
    EXPECT_EQ(calls, (std::vector<int>{0, 1, 2}));
  }
}

TEST_F(BookCacheUtilsTest, NonEpubReplacementRecoveryDistinguishesEveryRenameBoundary) {
  using Action = NonEpubReplacementRecovery;
  EXPECT_EQ(decideNonEpubReplacementRecovery(true, true, true, false), Action::CancelPending);
  EXPECT_EQ(decideNonEpubReplacementRecovery(true, true, false, true), Action::RestoreOld);
  EXPECT_EQ(decideNonEpubReplacementRecovery(true, true, true, true), Action::FinalizeReplacement);
  EXPECT_EQ(decideNonEpubReplacementRecovery(true, false, false, false), Action::CancelPending);
  EXPECT_EQ(decideNonEpubReplacementRecovery(true, false, true, false), Action::FinalizeReplacement);
  EXPECT_EQ(decideNonEpubReplacementRecovery(true, false, true, true), Action::FailClosed);
  EXPECT_EQ(decideNonEpubReplacementRecovery(false, false, false, true), Action::FailClosed);
  EXPECT_EQ(decideNonEpubReplacementRecovery(false, false, true, true), Action::FailClosed);
  EXPECT_EQ(decideNonEpubReplacementRecovery(false, false, true, false), Action::Nothing);
}

TEST_F(BookCacheUtilsTest, ReplacementNeverDeletesAPreexistingDiscardDirectoryByNameAlone) {
  const std::string existingDiscard = DISCARD_PATH;
  Storage.addDirectory(existingDiscard);
  Storage.setFile(existingDiscard + "/unknown.bin", bytes(77));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_EQ(Storage.file(existingDiscard + "/unknown.bin"), bytes(77));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
}

TEST_F(BookCacheUtilsTest, ReplacementUsesMonotonicArchiveSlotsBeyondFourWithoutOverwriting) {
  for (unsigned generation = 0; generation < 6; ++generation) {
    if (!Storage.exists(CACHE_PATH)) Storage.addDirectory(CACHE_PATH);
    put("progress.bin", static_cast<unsigned char>(80 + generation));

    ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

    const std::string archive = discardPathFor(generation);
    EXPECT_EQ(Storage.file(archive + "/progress.bin"), bytes(static_cast<unsigned char>(80 + generation)));
  }
}

TEST_F(BookCacheUtilsTest, ReplacementFailsClosedWhenAllBoundedArchiveSlotsAreOccupied) {
  for (unsigned slot = 0; slot < 256; ++slot) {
    const std::string archive = discardPathFor(slot);
    Storage.addDirectory(archive);
    Storage.setFile(archive + "/preserved.bin", bytes(static_cast<unsigned char>(slot)));
  }
  put("progress.bin", 87);

  EXPECT_FALSE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_TRUE(Storage.exists(CACHE_PATH));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/progress.bin"), bytes(87));
  EXPECT_EQ(Storage.file(discardPathFor(255) + "/preserved.bin"), bytes(255));
}

TEST_F(BookCacheUtilsTest, ReplacementPreservesUnexpectedClearStagingAndFailsClosed) {
  Storage.addDirectory(STAGING_PATH);
  Storage.setFile(std::string(STAGING_PATH) + "/unknown.bin", bytes(9));

  EXPECT_FALSE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_TRUE(Storage.exists(CACHE_PATH));
  EXPECT_EQ(Storage.file(std::string(STAGING_PATH) + "/unknown.bin"), bytes(9));
}

TEST_F(BookCacheUtilsTest, ReplacementPreservesCleanupWithAMalformedOwnershipMarker) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin", bytes(8));
  ASSERT_TRUE(Storage.rename(MOVE_STAGING_PATH, MOVE_CLEANUP_PATH));
  const std::string marker = std::string(MOVE_CLEANUP_PATH) + "/.crossvi_move_ready";
  const std::vector<unsigned char> malformed{0x01, 0x02, 0x03};
  Storage.setFile(marker, malformed);

  EXPECT_FALSE(resetBookCacheUserStateAfterReplacement(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));

  EXPECT_EQ(Storage.file(marker), malformed);
  EXPECT_TRUE(Storage.exists(MOVE_CLEANUP_PATH));
  // Unknown recovery bytes keep the canonical cache (and source-identity
  // barrier) active, so a retry cannot silently adopt the replacement.
  EXPECT_TRUE(Storage.exists(MOVED_CACHE_PATH));
}

TEST_F(BookCacheUtilsTest, MovesOnlyVerifiedUserStateAndLeavesTheSourceAuthoritative) {
  put("stats_v5.bin", 1);
  put("crossvi_reader_settings.bin", 2);
  put("progress.bin", 3);
  put("book.bin", 4);
  put("reader_settings.bin", 7);
  put("source_identity.bin", 8);
  put(".crossvi_replaced_clippings.bin", 11);
  put(".crossvi_replaced_bookmark.json", 12);
  const std::string sourceBookmark = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(sourceBookmark, bytes(6));
  Storage.addDirectory(std::string(CACHE_PATH) + "/sections");
  Storage.setFile(std::string(CACHE_PATH) + "/sections/0.bin", bytes(5));

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/stats_v5.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/book.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/stats_v5.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/crossvi_reader_settings.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/progress.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/reader_settings.bin").c_str()));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/source_identity.bin").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/.crossvi_replaced_clippings.bin").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/.crossvi_replaced_bookmark.json").c_str()));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_clippings.bin"), bytes(11));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), bytes(12));
  EXPECT_FALSE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/book.bin").c_str()));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/.crossvi_bookmark.json"), bytes(6));

  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(1));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/crossvi_reader_settings.bin"), bytes(2));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/progress.bin"), bytes(3));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/reader_settings.bin"), bytes(7));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/source_identity.bin"), bytes(8));
  EXPECT_FALSE(Storage.exists((std::string(MOVED_CACHE_PATH) + "/book.bin").c_str()));
  EXPECT_EQ(Storage.file(destinationBookmark), bytes(6));
  EXPECT_EQ(Storage.file(sourceBookmark), bytes(6));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_TRUE(Storage.exists((std::string(CACHE_PATH) + "/stats_v5.bin").c_str()));
  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(sourceBookmark.c_str()));
  EXPECT_EQ(Storage.file(destinationBookmark), bytes(6));
}

TEST_F(BookCacheUtilsTest, MovePreparationRejectsCachePathsThatAreNotExactBookKeys) {
  EXPECT_FALSE(prepareBookCacheUserStateMove("/Books", MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(prepareBookCacheUserStateMove("/.crosspoint/epub_wrong", MOVED_CACHE_PATH, SOURCE_BOOK_PATH,
                                             DESTINATION_BOOK_PATH));
  EXPECT_FALSE(
      prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH + "/nested", SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, HostileMoveMarkerCannotDeleteAnUnrelatedDirectory) {
  constexpr char MISSING_SOURCE[] = "/missing/source.epub";
  Storage.addDirectory("/Books");
  Storage.setFile("/Books/keep.epub", bytes(91));
  Storage.setFile(DESTINATION_BOOK_PATH, bytes(92));
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(MOVED_CACHE_PATH + "/.crossvi_move_ready",
                  moveMarker(MISSING_SOURCE, DESTINATION_BOOK_PATH, "/Books", MOVED_CACHE_PATH));

  EXPECT_FALSE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));

  EXPECT_TRUE(Storage.exists("/Books"));
  EXPECT_EQ(Storage.file("/Books/keep.epub"), bytes(91));
  EXPECT_TRUE(Storage.exists(MOVED_CACHE_PATH + "/.crossvi_move_ready"));
}

TEST_F(BookCacheUtilsTest, MoveTransactionPreservesTextProgressAcrossTxtToMarkdownRename) {
  constexpr char sourceBook[] = "/books/notes.txt";
  constexpr char destinationBook[] = "/read/notes.md";
  const std::string sourceCache = Txt(sourceBook, "/.crosspoint").getCachePath();
  const std::string destinationCache = Txt(destinationBook, "/.crosspoint").getCachePath();
  Storage.setFile(sourceBook, bytes(31));
  Storage.addDirectory(sourceCache);
  Storage.setFile(sourceCache + "/progress.bin", bytes(32));

  ASSERT_TRUE(prepareBookCacheUserStateMove(sourceCache, destinationCache, sourceBook, destinationBook));
  ASSERT_TRUE(Storage.rename(sourceBook, destinationBook));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(sourceCache, destinationCache, sourceBook, destinationBook));
  ASSERT_TRUE(completeBookCacheUserStateMove(sourceCache, destinationCache, sourceBook, destinationBook));

  EXPECT_EQ(Storage.file(destinationCache + "/progress.bin"), bytes(32));
  EXPECT_FALSE(Storage.exists(sourceCache.c_str()));
}

TEST_F(BookCacheUtilsTest, MoveCompletionArchivesUnknownSourceStateInsteadOfDeletingIt) {
  put("progress.bin", 31);
  put("book.bin", 32);
  put("future_reader_state.bin", 33);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/progress.bin"), bytes(31));
  EXPECT_FALSE(Storage.exists(DISCARD_PATH + "/book.bin"));
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/future_reader_state.bin"), bytes(33));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/progress.bin"), bytes(31));
}

TEST_F(BookCacheUtilsTest, MoveTransactionPreservesXtcProgress) {
  constexpr char sourceBook[] = "/books/story.xtc";
  constexpr char destinationBook[] = "/read/story.xtc";
  const std::string sourceCache = Xtc(sourceBook, "/.crosspoint").getCachePath();
  const std::string destinationCache = Xtc(destinationBook, "/.crosspoint").getCachePath();
  Storage.setFile(sourceBook, bytes(41));
  Storage.addDirectory(sourceCache);
  Storage.setFile(sourceCache + "/progress.bin", bytes(42));

  ASSERT_TRUE(prepareBookCacheUserStateMove(sourceCache, destinationCache, sourceBook, destinationBook));
  ASSERT_TRUE(Storage.rename(sourceBook, destinationBook));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(sourceCache, destinationCache, sourceBook, destinationBook));
  ASSERT_TRUE(completeBookCacheUserStateMove(sourceCache, destinationCache, sourceBook, destinationBook));

  EXPECT_EQ(Storage.file(destinationCache + "/progress.bin"), bytes(42));
  EXPECT_FALSE(Storage.exists(sourceCache.c_str()));
}

TEST_F(BookCacheUtilsTest, MoveTransactionRejectsCrossFormatAndForgedNonEpubCacheKeys) {
  constexpr char textBook[] = "/books/notes.txt";
  constexpr char xtcBook[] = "/read/notes.xtc";
  const std::string textCache = Txt(textBook, "/.crosspoint").getCachePath();
  const std::string xtcCache = Xtc(xtcBook, "/.crosspoint").getCachePath();
  Storage.setFile(textBook, bytes(51));
  Storage.addDirectory(textCache);

  EXPECT_FALSE(prepareBookCacheUserStateMove(textCache, xtcCache, textBook, xtcBook));
  EXPECT_FALSE(prepareBookCacheUserStateMove(textCache, "/.crosspoint/txt_forged", textBook, "/read/notes.md"));
  EXPECT_TRUE(Storage.exists(textBook));
  EXPECT_TRUE(Storage.exists(textCache));
}

TEST_F(BookCacheUtilsTest, OpenTimeRecoveryPublishesACompletedStateMove) {
  put("stats_v5.bin", 8);
  put("progress.bin", 9);
  const std::string sourceBookmark = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(sourceBookmark, bytes(10));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/progress.bin"), bytes(9));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(sourceBookmark.c_str()));
  EXPECT_EQ(Storage.file(destinationBookmark), bytes(10));
}

TEST_F(BookCacheUtilsTest, ReplacementDiscardsOwnedMoveStagingWithoutTouchingLiveSource) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  Storage.setFile(DESTINATION_BOOK_PATH, bytes(200));  // unrelated replacement at the prepared destination

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_TRUE(Storage.exists(SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists((std::string(MOVED_CACHE_PATH) + "/stats_v5.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, ReplacementAfterRenameDiscardsOldSourceDuplicates) {
  put("stats_v5.bin", 8);
  Storage.setFile(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH), bytes(10));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_EQ(Storage.file(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH)), emptyBookmarks());
  EXPECT_EQ(Storage.file(DISCARD_PATH + "/.crossvi_replaced_bookmark.json"), bytes(10));
}

TEST_F(BookCacheUtilsTest, ReplacingMoveSourceCancelsOwnedPreRenameDestinationStaging) {
  put("stats_v5.bin", 8);
  Storage.setFile(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH), bytes(13));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.remove(SOURCE_BOOK_PATH));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_EQ(Storage.file(BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH)), emptyBookmarks());
  const std::string stagedArchive = siblingPathWithPrefix(MOVE_STAGING_PATH, ".crossvi_discard_");
  EXPECT_EQ(Storage.file(stagedArchive + "/.crossvi_bookmark.json"), bytes(13));
  EXPECT_TRUE(Storage.exists(stagedArchive + "/.crossvi_move_ready"));
}

TEST_F(BookCacheUtilsTest, ReplacingMoveSourcePreservesAValidMarkerUnderTheWrongStagingKey) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  constexpr char WRONG_STAGING_PATH[] = "/.crosspoint/.crossvi_move_unrelated";
  ASSERT_TRUE(Storage.rename(MOVE_STAGING_PATH, WRONG_STAGING_PATH));
  ASSERT_TRUE(Storage.remove(SOURCE_BOOK_PATH));

  ASSERT_TRUE(resetBookCacheUserStateAfterReplacement(CACHE_PATH, SOURCE_BOOK_PATH));

  EXPECT_TRUE(Storage.exists(WRONG_STAGING_PATH));
  EXPECT_FALSE(Storage.exists(BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH).c_str()));
}

TEST_F(BookCacheUtilsTest, DestinationOpenFinishesCleanupAfterPublishReset) {
  put("stats_v5.bin", 8);
  const std::string sourceBookmark = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(sourceBookmark, bytes(10));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.exists((std::string(MOVED_CACHE_PATH) + "/.crossvi_move_ready").c_str()));
  ASSERT_TRUE(Storage.exists(CACHE_PATH));
  ASSERT_TRUE(Storage.exists(sourceBookmark.c_str()));

  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(sourceBookmark.c_str()));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  EXPECT_EQ(Storage.file(destinationBookmark), bytes(10));
  EXPECT_FALSE(Storage.exists((std::string(MOVED_CACHE_PATH) + "/.crossvi_move_ready").c_str()));
}

TEST_F(BookCacheUtilsTest, PreparedStateMoveNeverOverwritesAnExistingDestination) {
  put("stats_v5.bin", 1);
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin", bytes(7));

  EXPECT_FALSE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(7));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, RecoveryNeverPublishesBeforeTheBookRenameBoundary) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_FALSE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVED_CACHE_PATH));
  EXPECT_TRUE(Storage.exists(MOVE_STAGING_PATH));
  EXPECT_TRUE(Storage.exists(SOURCE_BOOK_PATH));
}

TEST_F(BookCacheUtilsTest, RecoveryMergesIntoDerivedOnlyCacheCreatedAfterBookRename) {
  put("stats_v5.bin", 8);
  Storage.setFile(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH), bytes(10));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/book.bin", bytes(11));
  Storage.addDirectory(std::string(MOVED_CACHE_PATH) + "/sections");
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/sections/0.bin", bytes(12));

  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/book.bin"), bytes(11));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/sections/0.bin"), bytes(12));
  EXPECT_EQ(Storage.file(BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH)), bytes(10));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));
}

TEST_F(BookCacheUtilsTest, CleanupFailureAfterAtomicMergeNeverBlocksTheRecoveredBook) {
  put("stats_v5.bin", 8);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  Storage.addDirectory(MOVED_CACHE_PATH);
  Storage.setFile(std::string(MOVED_CACHE_PATH) + "/book.bin", bytes(11));
  Storage.failDeletePathTimes(MOVE_CLEANUP_PATH, 2);

  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
  EXPECT_TRUE(Storage.exists(MOVE_CLEANUP_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_STAGING_PATH));

  // Model a reset after recursive cleanup removed the marker first.
  ASSERT_TRUE(Storage.remove((std::string(MOVE_CLEANUP_PATH) + "/.crossvi_move_ready").c_str()));
  ASSERT_TRUE(recoverBookCacheUserState(MOVED_CACHE_PATH, DESTINATION_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(MOVE_CLEANUP_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVED_CACHE_PATH) + "/stats_v5.bin"), bytes(8));
}

TEST_F(BookCacheUtilsTest, InterruptedMarkerlessPrepareIsSafelyRebuilt) {
  put("stats_v5.bin", 3);
  put("crossvi_reader_settings.bin", 4);
  Storage.addDirectory(MOVE_STAGING_PATH);
  Storage.setFile(std::string(MOVE_STAGING_PATH) + "/stats_v5.bin", bytes(3));

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/stats_v5.bin"), bytes(3));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/crossvi_reader_settings.bin"), bytes(4));
  EXPECT_TRUE(Storage.exists((std::string(MOVE_STAGING_PATH) + "/.crossvi_move_ready").c_str()));
}

TEST_F(BookCacheUtilsTest, PreparedCopyIsRebuiltWhenSourceStateChangesBeforeBookRename) {
  put("stats_v5.bin", 3);
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  put("stats_v5.bin", 7);

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/stats_v5.bin"), bytes(7));
  EXPECT_TRUE(Storage.exists(SOURCE_BOOK_PATH));
  EXPECT_FALSE(Storage.exists(DESTINATION_BOOK_PATH));
}

TEST_F(BookCacheUtilsTest, UnexpectedInterruptedMoveContentIsPreserved) {
  put("stats_v5.bin", 3);
  Storage.addDirectory(MOVE_STAGING_PATH);
  Storage.setFile(std::string(MOVE_STAGING_PATH) + "/unknown.bin", bytes(9));

  EXPECT_FALSE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/unknown.bin"), bytes(9));
}

TEST_F(BookCacheUtilsTest, BookmarkKeysDoNotFlattenDistinctBookPathsTogether) {
  EXPECT_NE(BookmarkUtil::getBookmarkPath("/Books/Foo_Bar.epub"), BookmarkUtil::getBookmarkPath("/Books/Foo/Bar.epub"));
  EXPECT_EQ(BookmarkUtil::getLegacyBookmarkPath("/Books/Foo_Bar.epub"),
            BookmarkUtil::getLegacyBookmarkPath("/Books/Foo/Bar.epub"));
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkQuarantinePreservesBytesAndShadowsLegacyFallback) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string legacy = BookmarkUtil::getLegacyBookmarkPath(SOURCE_BOOK_PATH);
  const auto canonicalBytes = bytes(41);
  const auto legacyBytes = bytes(51);
  Storage.setFile(canonical, canonicalBytes);
  Storage.setFile(legacy, legacyBytes);

  ASSERT_TRUE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), canonicalBytes);
  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(canonical));
  EXPECT_EQ(Storage.file(legacy), legacyBytes);
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkTombstonePublicationRetriesWithoutLosingOriginalBytes) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string pending = canonical + ".crossvi_replacement.tmp";
  const auto original = bytes(61);
  Storage.setFile(canonical, original);
  // Archive succeeds, atomic tombstone publication fails.
  Storage.failRenameOnCall(2);

  EXPECT_FALSE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));
  EXPECT_FALSE(Storage.exists(canonical.c_str()));
  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(pending));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), original);

  ASSERT_TRUE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));
  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(canonical));
  EXPECT_FALSE(Storage.exists(pending.c_str()));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), original);
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkRetriesAfterShortTombstoneWrite) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string pending = canonical + ".crossvi_replacement.tmp";
  const auto original = bytes(62);
  Storage.setFile(canonical, original);
  Storage.shortWriteOnce();

  EXPECT_FALSE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));
  EXPECT_FALSE(BookmarkUtil::isEmptyBookmarkFile(pending));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), original);

  ASSERT_TRUE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));
  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(canonical));
  EXPECT_FALSE(Storage.exists(pending.c_str()));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), original);
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkRetriesAfterTombstoneSyncFailure) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string pending = canonical + ".crossvi_replacement.tmp";
  const auto original = bytes(63);
  Storage.setFile(canonical, original);
  Storage.failSyncOnce();

  EXPECT_FALSE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));
  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(pending));

  ASSERT_TRUE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));
  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(canonical));
  EXPECT_FALSE(Storage.exists(pending.c_str()));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), original);
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkRebuildsTruncatedOwnedPendingFile) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string pending = canonical + ".crossvi_replacement.tmp";
  const auto original = bytes(64);
  Storage.setFile(canonical, original);
  ASSERT_TRUE(
      Storage.rename(canonical.c_str(), (std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json").c_str()));
  Storage.setFile(pending, bytes(65));

  ASSERT_TRUE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));
  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(canonical));
  EXPECT_FALSE(Storage.exists(pending.c_str()));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), original);
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkDropsInvalidPendingBesideVerifiedCanonicalTombstone) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string pending = canonical + ".crossvi_replacement.tmp";
  ASSERT_TRUE(BookmarkUtil::writeEmptyCanonicalBookmark(SOURCE_BOOK_PATH));
  Storage.setFile(pending, bytes(66));

  ASSERT_TRUE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));

  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(canonical));
  EXPECT_FALSE(Storage.exists(pending.c_str()));
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkUsesFreshBoundedSlotWithoutOverwritingAnOrphan) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  Storage.setFile(canonical, bytes(71));
  Storage.setFile(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json", bytes(72));

  ASSERT_TRUE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));

  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json"), bytes(72));
  EXPECT_EQ(Storage.file(std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json.2"), bytes(71));
  EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(canonical));
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkUsesMonotonicSlotsBeyondFourWithoutOverwriting) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string archiveBase = std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json";
  for (unsigned generation = 0; generation < 6; ++generation) {
    const auto original = bytes(static_cast<unsigned char>(90 + generation));
    Storage.setFile(canonical, original);

    ASSERT_TRUE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));

    std::string archive = archiveBase;
    if (generation > 0) archive += "." + std::to_string(generation + 1);
    EXPECT_EQ(Storage.file(archive), original);
    EXPECT_TRUE(BookmarkUtil::isEmptyBookmarkFile(canonical));
  }
}

TEST_F(BookCacheUtilsTest, ReplacementBookmarkFailsClosedWhenAllBoundedSlotsAreOccupied) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string archiveBase = std::string(CACHE_PATH) + "/.crossvi_replaced_bookmark.json";
  for (unsigned slot = 0; slot < 256; ++slot) {
    std::string archive = archiveBase;
    if (slot > 0) archive += "." + std::to_string(slot + 1);
    Storage.setFile(archive, bytes(static_cast<unsigned char>(slot)));
  }
  const auto original = bytes(97);
  Storage.setFile(canonical, original);

  EXPECT_FALSE(BookmarkUtil::quarantineCanonicalForReplacement(SOURCE_BOOK_PATH, CACHE_PATH));

  EXPECT_EQ(Storage.file(canonical), original);
  EXPECT_EQ(Storage.file(archiveBase + ".256"), bytes(255));
}

TEST_F(BookCacheUtilsTest, ReplacementShadowsButNeverDeletesAnAmbiguousLegacyBookmark) {
  const std::string legacy = BookmarkUtil::getLegacyBookmarkPath(SOURCE_BOOK_PATH);
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  Storage.setFile(legacy, bytes(14));

  ASSERT_TRUE(BookmarkUtil::ensureLegacyBookmarkShadowed(SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(canonical), emptyBookmarks());
  EXPECT_EQ(Storage.file(legacy), bytes(14));
}

TEST_F(BookCacheUtilsTest, LegacyShadowOverwritesStaleCanonicalBookmarksWithVerifiedEmptyState) {
  const std::string legacy = BookmarkUtil::getLegacyBookmarkPath(SOURCE_BOOK_PATH);
  const std::string canonical = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  Storage.setFile(legacy, bytes(14));
  Storage.setFile(canonical, bytes(22));

  ASSERT_TRUE(BookmarkUtil::ensureLegacyBookmarkShadowed(SOURCE_BOOK_PATH));
  EXPECT_EQ(Storage.file(canonical), emptyBookmarks());
  EXPECT_EQ(Storage.file(legacy), bytes(14));
}

TEST_F(BookCacheUtilsTest, EmptyDestinationShadowCanBeReplacedByMovedSourceBookmarks) {
  const std::string source = BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destination = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(source, bytes(21));
  ASSERT_TRUE(BookmarkUtil::writeEmptyCanonicalBookmark(DESTINATION_BOOK_PATH));

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_EQ(Storage.file(destination), bytes(21));
}

TEST_F(BookCacheUtilsTest, DestinationLegacyIsShadowedWhenMovedSourceHasNoBookmarks) {
  const std::string destinationLegacy = BookmarkUtil::getLegacyBookmarkPath(DESTINATION_BOOK_PATH);
  const std::string destination = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(destinationLegacy, bytes(31));

  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  EXPECT_EQ(Storage.file(std::string(MOVE_STAGING_PATH) + "/.crossvi_bookmark.json"), emptyBookmarks());
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_EQ(Storage.file(destination), emptyBookmarks());
  EXPECT_EQ(Storage.file(destinationLegacy), bytes(31));
}

TEST_F(BookCacheUtilsTest, LegacyBookmarkIsCopiedButNeverDeletedAsOwnedState) {
  const std::string legacySource = BookmarkUtil::getLegacyBookmarkPath(SOURCE_BOOK_PATH);
  const std::string destination = BookmarkUtil::getBookmarkPath(DESTINATION_BOOK_PATH);
  Storage.setFile(legacySource, bytes(14));
  ASSERT_TRUE(prepareBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(Storage.rename(SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(finalizeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));
  ASSERT_TRUE(completeBookCacheUserStateMove(CACHE_PATH, MOVED_CACHE_PATH, SOURCE_BOOK_PATH, DESTINATION_BOOK_PATH));

  EXPECT_EQ(Storage.file(destination), bytes(14));
  EXPECT_EQ(Storage.file(legacySource), bytes(14));
  EXPECT_EQ(Storage.file(BookmarkUtil::getBookmarkPath(SOURCE_BOOK_PATH)), emptyBookmarks());
}

}  // namespace

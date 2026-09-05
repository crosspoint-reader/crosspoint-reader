#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "LibraryFormat.h"

using namespace library;

namespace {

// A header for N books whose sections are laid out consistently, i.e. one that
// validateHeader() must accept. Tests then damage exactly one thing.
ClixHeader makeHeader(const uint16_t books, const uint32_t folderBytes = 300, const uint32_t nameBytes = 0) {
  ClixHeader h{};
  memcpy(h.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  h.formatVersion = CLIX_FORMAT_VERSION;
  h.foldVersion = CLIX_FOLD_VERSION;
  h.bookCount = books;
  h.folderCount = 4;
  layoutSections(h, folderBytes, nameBytes == 0 ? books * 80u : nameBytes);
  return h;
}

}  // namespace

TEST(LibraryFormat, StructSizesAreFrozen) {
  // These are the on-disk contract. A compiler that pads them silently would
  // produce an index this build writes and no other build can read.
  EXPECT_EQ(sizeof(ClixHeader), 64u);
  EXPECT_EQ(sizeof(ClixRecord), 128u);
  EXPECT_EQ(sizeof(ClixFolderHeader), 1u);
}

TEST(LibraryFormat, RecordsTileSectorsExactly) {
  // The whole streaming design rests on this: 32 records fill a 4096-byte
  // buffer with nothing left over, so a scan never has to handle a record split
  // across two reads.
  EXPECT_EQ(4096u % sizeof(ClixRecord), 0u);
  EXPECT_EQ(4096u / sizeof(ClixRecord), 32u);
  EXPECT_EQ(CLIX_ALIGN % sizeof(ClixRecord), 0u);
}

TEST(LibraryFormat, EverySectionStartsOnASectorBoundary) {
  for (const uint16_t n : {uint16_t{0}, uint16_t{1}, uint16_t{3}, uint16_t{60}, uint16_t{200}, uint16_t{2000}}) {
    const ClixHeader h = makeHeader(n, 29u * 4u);
    EXPECT_EQ(h.folderStart % CLIX_ALIGN, 0u) << "n=" << n;
    EXPECT_EQ(h.recordStart % CLIX_ALIGN, 0u) << "n=" << n;
    EXPECT_EQ(h.permStart % CLIX_ALIGN, 0u) << "n=" << n;
    EXPECT_EQ(h.nameStart % CLIX_ALIGN, 0u) << "n=" << n;
  }
}

TEST(LibraryFormat, SectionsDoNotOverlap) {
  const ClixHeader h = makeHeader(200, 29u * 50u);
  EXPECT_GE(h.folderStart, sizeof(ClixHeader));
  EXPECT_GE(h.recordStart, h.folderStart + h.folderLen);
  EXPECT_GE(h.permStart, h.recordStart + 200u * sizeof(ClixRecord));
  EXPECT_GE(h.nameStart, h.permStart + 200u * 2u * sizeof(uint16_t));
  EXPECT_EQ(h.selfSize, h.nameStart + h.nameLen);
}

TEST(LibraryFormat, RecordOffsetsAreAlignedAndOrdered) {
  const ClixHeader h = makeHeader(64, 116);
  EXPECT_EQ(recordOffset(h, 0), h.recordStart);
  EXPECT_EQ(recordOffset(h, 1), h.recordStart + 128u);
  EXPECT_EQ(recordOffset(h, 63), h.recordStart + 63u * 128u);
  // Every 4th record starts on a sector boundary, by construction.
  for (uint16_t k = 0; k < 64; k += 4) EXPECT_EQ(recordOffset(h, k) % CLIX_ALIGN, 0u);
}

TEST(LibraryFormat, PermutationArraysDoNotOverlapEachOther) {
  const ClixHeader h = makeHeader(100, 116);
  EXPECT_EQ(authorOrderOffset(h, 0), h.permStart);
  EXPECT_EQ(authorOrderOffset(h, 99), h.permStart + 198u);
  EXPECT_EQ(arrivalOrderOffset(h, 0), h.permStart + 200u);
  EXPECT_GT(arrivalOrderOffset(h, 0), authorOrderOffset(h, h.bookCount - 1));
  EXPECT_EQ(seriesOrderOffset(h, 0), h.permStart + 400u);
  EXPECT_GT(seriesOrderOffset(h, 0), arrivalOrderOffset(h, h.bookCount - 1));
}

TEST(LibraryFormat, SizeArithmeticMatchesTheSpecTable) {
  // Spec section 3.7, the 200-book row, now carrying series: 512 header + 1536
  // folders + 25600 records + 1536 permutations + 20 series entries + 800
  // series refs + 16000 names.
  ClixHeader h{};
  memcpy(h.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  h.formatVersion = CLIX_FORMAT_VERSION;
  h.foldVersion = CLIX_FOLD_VERSION;
  h.bookCount = 200;
  h.seriesCount = 20;
  layoutSections(h, 29u * 50u, 80u * 200u);
  EXPECT_EQ(h.folderStart, 512u);
  EXPECT_EQ(h.recordStart, 2048u);
  EXPECT_EQ(h.permStart, 2048u + 25600u);
  // Each section is rounded up to the next sector, so the series table's 1280
  // bytes and the refs' 800 both cost a little padding.
  EXPECT_EQ(h.seriesStart, 29184u);
  EXPECT_EQ(h.seriesRefStart, 30720u);
  EXPECT_EQ(h.nameStart, 31744u);
  EXPECT_EQ(h.selfSize, 47744u);
}

TEST(LibraryFormat, SeriesSectionsAreEmptyWhenNoBookNamesASeries) {
  ClixHeader h{};
  h.bookCount = 10;
  h.seriesCount = 0;
  layoutSections(h, 0, 100);
  // With no entries the table takes no room, so the refs follow immediately.
  EXPECT_EQ(h.seriesRefStart, h.seriesStart);
  EXPECT_EQ(h.seriesStart % CLIX_ALIGN, 0u);
}

TEST(LibraryFormat, SeriesEntriesTileSectorsExactly) {
  const ClixHeader h = makeHeader(64, 116);
  EXPECT_EQ(seriesEntryOffset(h, 0), h.seriesStart);
  EXPECT_EQ(seriesEntryOffset(h, 1), h.seriesStart + 64u);
  // 8 entries fill a sector, so every 8th starts on a boundary.
  for (uint16_t k = 0; k < 32; k += 8) EXPECT_EQ(seriesEntryOffset(h, k) % CLIX_ALIGN, 0u);
}

TEST(LibraryFormat, SeriesRefsAreParallelToTheRecords) {
  const ClixHeader h = makeHeader(64, 116);
  EXPECT_EQ(seriesRefOffset(h, 0), h.seriesRefStart);
  EXPECT_EQ(seriesRefOffset(h, 63), h.seriesRefStart + 63u * 4u);
}

TEST(LibraryFormatValidation, AcceptsAWellFormedHeader) {
  const ClixHeader h = makeHeader(60, 116);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);
}

TEST(LibraryFormatValidation, RejectsBadMagic) {
  ClixHeader h = makeHeader(60, 116);
  h.magic[3] = '2';
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::BadMagic);
  ClixHeader zero{};
  EXPECT_EQ(validateHeader(zero, 0), ClixValidity::BadMagic);
}

TEST(LibraryFormatValidation, RejectsUnknownVersionsSeparately) {
  ClixHeader h = makeHeader(60, 116);
  h.formatVersion = library::CLIX_FORMAT_VERSION + 1;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::UnknownFormatVersion);

  // A fold change is recoverable — firstSeen is preserved across the rebuild —
  // so it must be distinguishable from an unreadable format.
  h = makeHeader(60, 116);
  h.foldVersion = CLIX_FOLD_VERSION + 1;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::StaleFoldVersion);
  EXPECT_EQ(validateHeaderStructure(h, h.selfSize), ClixValidity::Ok);

  // Reconciliation may ignore only the fold version, never damaged layout.
  EXPECT_EQ(validateHeaderStructure(h, h.selfSize - 1), ClixValidity::SizeMismatch);
}

TEST(LibraryFormatValidation, RejectsLengthsBeyondTheFile) {
  // folderLen and nameLen are attacker bytes; near-2^32 values used to wrap
  // the section sums and re-derive the same wrapped layout on both sides.
  ClixHeader h = makeHeader(60, 116);
  h.folderLen = 0xFFFFFF00u;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
  h = makeHeader(60, 116);
  h.nameLen = 0xFFFFFF00u;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, RejectsTruncationInBothDirections) {
  const ClixHeader h = makeHeader(60, 116);
  // Power loss mid-build: the file is short.
  EXPECT_EQ(validateHeader(h, h.selfSize - 1), ClixValidity::SizeMismatch);
  EXPECT_EQ(validateHeader(h, h.selfSize / 2), ClixValidity::SizeMismatch);
  EXPECT_EQ(validateHeader(h, 0), ClixValidity::SizeMismatch);
  // Longer than declared is equally wrong: a stale tail from a previous build.
  EXPECT_EQ(validateHeader(h, h.selfSize + 512), ClixValidity::SizeMismatch);
}

TEST(LibraryFormatValidation, RejectsTamperedOffsets) {
  ClixHeader h = makeHeader(60, 116);
  h.recordStart += CLIX_ALIGN;  // plausible, aligned, and wrong
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, RejectsAnImpossibleBookCount) {
  ClixHeader h = makeHeader(60, 116);
  h.bookCount = CLIX_MAX_RECORDS + 1;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::CountOutOfRange);
}

TEST(LibraryFormatValidation, AcceptsAnEmptyLibrary) {
  // A card with no books must produce a valid index, not a rebuild every boot.
  ClixHeader h = makeHeader(0, 0, 1);
  h.nameLen = 0;
  layoutSections(h, 0, 0);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);
  EXPECT_EQ(h.selfSize, CLIX_ALIGN);
}

TEST(LibraryHeaderFlags, DedupDegradationIsPersistedWithoutChangingTheLayout) {
  ClixHeader h = makeHeader(60, 116);
  h.flags = CLIX_FLAG_DEDUP_DEGRADED;

  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);
  EXPECT_NE(h.flags & CLIX_FLAG_DEDUP_DEGRADED, 0);
  EXPECT_EQ(sizeof(ClixHeader), 64u);
}

TEST(LibraryFormat, ByteImageIsStableAcrossBuilds) {
  // Guards the packing itself: if a compiler ever inserts padding, these field
  // offsets move and the on-disk format silently forks.
  EXPECT_EQ(offsetof(ClixRecord, nameOff), 0u);
  EXPECT_EQ(offsetof(ClixRecord, fileSize), 4u);
  EXPECT_EQ(offsetof(ClixRecord, firstSeen), 8u);
  EXPECT_EQ(offsetof(ClixRecord, folderId), 10u);
  EXPECT_EQ(offsetof(ClixRecord, nameLen), 12u);
  EXPECT_EQ(offsetof(ClixRecord, foldLen), 13u);
  EXPECT_EQ(offsetof(ClixRecord, authorKeyLen), 14u);
  EXPECT_EQ(offsetof(ClixRecord, fold), 16u);
  EXPECT_EQ(offsetof(ClixRecord, authorKey), 112u);
  EXPECT_EQ(offsetof(ClixRecord, reserved), 124u);

  EXPECT_EQ(offsetof(ClixHeader, bookCount), 8u);
  EXPECT_EQ(offsetof(ClixHeader, seriesCount), 14u);
  EXPECT_EQ(offsetof(ClixHeader, folderStart), 16u);
  EXPECT_EQ(offsetof(ClixHeader, seriesStart), 32u);
  EXPECT_EQ(offsetof(ClixHeader, seriesRefStart), 36u);
  EXPECT_EQ(offsetof(ClixHeader, selfSize), 48u);
  EXPECT_EQ(offsetof(ClixHeader, knownSeriesCount), 52u);

  EXPECT_EQ(offsetof(ClixSeriesEntry, bookCount), 0u);
  EXPECT_EQ(offsetof(ClixSeriesEntry, nameLen), 2u);
  EXPECT_EQ(offsetof(ClixSeriesEntry, name), 3u);
  EXPECT_EQ(offsetof(ClixSeriesRef, seriesId), 0u);
  EXPECT_EQ(offsetof(ClixSeriesRef, seriesIndex), 2u);
}

TEST(LibraryFormatValidation, RejectsMoreSeriesThanBooks) {
  ClixHeader h = makeHeader(10, 116);
  h.seriesCount = 11;
  layoutSections(h, h.folderLen, h.nameLen);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::CountOutOfRange);
}

TEST(LibraryFormatValidation, RejectsGroupedBooksWithoutAnySeries) {
  ClixHeader h = makeHeader(10, 116);
  h.seriesCount = 0;
  h.knownSeriesCount = 4;
  layoutSections(h, h.folderLen, h.nameLen);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::CountOutOfRange);
}

TEST(LibraryFormatValidation, RejectsMoreSeriesThanGroupedBooks) {
  ClixHeader h = makeHeader(10, 116);
  h.seriesCount = 5;
  h.knownSeriesCount = 4;
  layoutSections(h, h.folderLen, h.nameLen);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::CountOutOfRange);
}

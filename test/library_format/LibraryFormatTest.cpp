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
  h.metadataEnabled = 1;
  h.groupKind = static_cast<uint8_t>(GroupKind::Series);
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
  EXPECT_EQ(CLIX_FORMAT_VERSION, 3u);
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
  EXPECT_EQ(groupOrderOffset(h, 0), h.permStart + 400u);
  EXPECT_GT(groupOrderOffset(h, 0), arrivalOrderOffset(h, h.bookCount - 1));
}

TEST(LibraryFormat, SizeArithmeticMatchesTheSpecTable) {
  ClixHeader h{};
  memcpy(h.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  h.formatVersion = CLIX_FORMAT_VERSION;
  h.foldVersion = CLIX_FOLD_VERSION;
  h.bookCount = 200;
  h.groupCount = 20;
  layoutSections(h, 29u * 50u, 80u * 200u);
  EXPECT_EQ(h.folderStart, 512u);
  EXPECT_EQ(h.recordStart, 2048u);
  EXPECT_EQ(h.permStart, 2048u + 25600u);
  // Round the 1280-byte group table and 800-byte references up to whole sectors.
  EXPECT_EQ(h.groupStart, 29184u);
  EXPECT_EQ(h.groupRefStart, 30720u);
  EXPECT_EQ(h.nameStart, 31744u);
  EXPECT_EQ(h.selfSize, 47744u);
}

TEST(LibraryFormat, GroupSectionsAreEmptyWhenNoBookIsGrouped) {
  ClixHeader h{};
  h.bookCount = 10;
  h.groupCount = 0;
  layoutSections(h, 0, 100);
  EXPECT_EQ(h.groupRefStart, h.groupStart);
  EXPECT_EQ(h.groupStart % CLIX_ALIGN, 0u);
}

TEST(LibraryFormat, GroupEntriesTileSectorsExactly) {
  const ClixHeader h = makeHeader(64, 116);
  EXPECT_EQ(groupEntryOffset(h, 0), h.groupStart);
  EXPECT_EQ(groupEntryOffset(h, 1), h.groupStart + 64u);
  for (uint16_t k = 0; k < 32; k += 8) EXPECT_EQ(groupEntryOffset(h, k) % CLIX_ALIGN, 0u);
}

TEST(LibraryFormat, GroupRefsAreParallelToTheRecords) {
  const ClixHeader h = makeHeader(64, 116);
  EXPECT_EQ(groupRefOffset(h, 0), h.groupRefStart);
  EXPECT_EQ(groupRefOffset(h, 63), h.groupRefStart + 63u * 4u);
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

TEST(LibraryFormatValidation, RejectsInvalidMetadataMode) {
  ClixHeader h = makeHeader(60, 116);
  h.metadataEnabled = 2;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
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
  EXPECT_EQ(offsetof(ClixRecord, metadataStatus), 15u);
  EXPECT_EQ(offsetof(ClixRecord, fold), 16u);
  EXPECT_EQ(offsetof(ClixRecord, authorKey), 112u);
  EXPECT_EQ(offsetof(ClixRecord, modificationTime), 124u);

  EXPECT_EQ(offsetof(ClixHeader, metadataEnabled), 7u);
  EXPECT_EQ(offsetof(ClixHeader, bookCount), 8u);
  EXPECT_EQ(offsetof(ClixHeader, groupCount), 14u);
  EXPECT_EQ(offsetof(ClixHeader, folderStart), 16u);
  EXPECT_EQ(offsetof(ClixHeader, groupStart), 32u);
  EXPECT_EQ(offsetof(ClixHeader, groupRefStart), 36u);
  EXPECT_EQ(offsetof(ClixHeader, selfSize), 48u);
  EXPECT_EQ(offsetof(ClixHeader, groupedCount), 52u);
  EXPECT_EQ(offsetof(ClixHeader, groupKind), 54u);

  EXPECT_EQ(offsetof(ClixGroupEntry, bookCount), 0u);
  EXPECT_EQ(offsetof(ClixGroupEntry, nameLen), 2u);
  EXPECT_EQ(offsetof(ClixGroupEntry, name), 3u);
  EXPECT_EQ(offsetof(ClixGroupRef, groupId), 0u);
  EXPECT_EQ(offsetof(ClixGroupRef, position), 2u);
}

TEST(LibraryFormatValidation, RejectsMoreGroupsThanBooks) {
  ClixHeader h = makeHeader(10, 116);
  h.groupCount = 11;
  layoutSections(h, h.folderLen, h.nameLen);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::CountOutOfRange);
}

TEST(LibraryFormatValidation, RejectsGroupedBooksWithoutAnyGroup) {
  ClixHeader h = makeHeader(10, 116);
  h.groupCount = 0;
  h.groupedCount = 4;
  layoutSections(h, h.folderLen, h.nameLen);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::CountOutOfRange);
}

TEST(LibraryFormatValidation, RejectsMoreGroupsThanGroupedBooks) {
  ClixHeader h = makeHeader(10, 116);
  h.groupCount = 5;
  h.groupedCount = 4;
  layoutSections(h, h.folderLen, h.nameLen);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::CountOutOfRange);
}

TEST(LibraryFormatValidation, RejectsAnUnknownGroupKind) {
  ClixHeader h = makeHeader(10, 116);
  h.groupKind = GROUP_KIND_COUNT;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
  h.groupKind = 0xFF;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, RejectsGroupCountsWithoutAGroupKind) {
  ClixHeader h = makeHeader(10, 116);
  h.groupKind = static_cast<uint8_t>(GroupKind::None);
  h.groupCount = 2;
  h.groupedCount = 4;
  layoutSections(h, h.folderLen, h.nameLen);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);

  h.groupCount = 0;
  layoutSections(h, h.folderLen, h.nameLen);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, RejectsAGroupKindWithoutMetadata) {
  ClixHeader h = makeHeader(10, 116);
  h.metadataEnabled = 0;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);

  h.groupKind = static_cast<uint8_t>(GroupKind::None);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);
}

TEST(LibraryFormatValidation, AcceptsEveryKnownGroupKind) {
  ClixHeader h = makeHeader(10, 116);
  h.groupKind = static_cast<uint8_t>(GroupKind::None);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);

  for (const GroupKind kind : {GroupKind::Series, GroupKind::Publisher, GroupKind::Language, GroupKind::Subject}) {
    h = makeHeader(10, 116);
    h.groupKind = static_cast<uint8_t>(kind);
    h.groupCount = 2;
    h.groupedCount = 4;
    layoutSections(h, h.folderLen, h.nameLen);
    EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok) << "kind=" << static_cast<unsigned>(kind);
  }
}

TEST(LibraryFormatValidation, AcceptsADegradedGroupKindWithNoTable) {
  ClixHeader h = makeHeader(10, 116);
  h.flags = CLIX_FLAG_GROUPS_DEGRADED;
  ASSERT_EQ(h.groupCount, 0);
  ASSERT_EQ(h.groupedCount, 0);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);
}

TEST(LibraryFormatValidation, RejectsTheGroupsDegradedFlagWithoutAKind) {
  ClixHeader h = makeHeader(10, 116);
  h.groupKind = static_cast<uint8_t>(GroupKind::None);
  h.flags = CLIX_FLAG_GROUPS_DEGRADED;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, RejectsTheGroupsDegradedFlagBesideATable) {
  ClixHeader h = makeHeader(10, 116);
  h.groupCount = 2;
  h.groupedCount = 4;
  layoutSections(h, h.folderLen, h.nameLen);
  ASSERT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);

  h.flags = CLIX_FLAG_GROUPS_DEGRADED;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

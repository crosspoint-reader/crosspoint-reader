// The glyph store's on-flash format and its install-time layout planner.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "GlyphStoreFormat.h"

using namespace glyphstore;

namespace {

constexpr uint32_t kX3UsableBytes = 3538944 - 65536;  // the X3's spiffs partition below the language slot: 53 pages

PlanInput input(const uint8_t mode, const uint8_t styles, const uint32_t styleBytes, const uint32_t freePages = 32) {
  PlanInput in{};
  in.mode = mode;
  in.styleCount = styles;
  for (uint8_t s = 0; s < styles && s < kMaxStyles; ++s) in.styleBytes[s] = styleBytes;
  in.usableBytes = kX3UsableBytes;
  in.freeMapPages = freePages;
  return in;
}

}  // namespace

TEST(GlyphStoreFormat, KeysNeverLookErased) {
  const uint32_t largest = makeKey(0x10FFFF, 3, kSizeEntries - 1);
  EXPECT_TRUE(isGlyphKey(largest));
  EXPECT_NE(kErased, largest);
  EXPECT_FALSE(isGlyphKey(kErased));
  EXPECT_EQ(0x10FFFFu, largest & 0x1FFFFFu);
  EXPECT_EQ(3u, (largest >> 21) & 0x3u);
  EXPECT_EQ(kSizeEntries - 1, keySizeIndex(largest));
  EXPECT_EQ(0u, largest >> 28);
}

TEST(GlyphStoreFormat, NoGlyphIsAValueOnlyACompleteWriteProduces) {
  // A cut leaves extra 1 bits; only a write that lands every bit reads as 0.
  EXPECT_EQ(0u, kNoGlyph);
  EXPECT_GE(kRecordHeaderBytes, 1u);  // a record slot holds a dataOffset, which is never 0
}

TEST(GlyphStoreFormat, HomeSlotMatchesFibonacciHashingForPowersOfTwo) {
  for (const uint32_t bits : {10u, 13u, 15u}) {
    const uint32_t slots = 1u << bits;
    for (uint32_t cp = 0x20; cp < 0x2000; cp += 7) {
      const uint32_t key = makeKey(cp, cp % 4, cp % 16);
      EXPECT_EQ((key * 2654435761u) >> (32 - bits), homeSlot(key, slots));
    }
  }
}

TEST(GlyphStoreFormat, HomeSlotStaysInRangeForAnySlotCount) {
  const uint32_t slots = 3 * kSlotsPerPage;  // three pages: not a power of two
  for (uint32_t cp = 0; cp < 0x30000; cp += 13) EXPECT_LT(homeSlot(makeKey(cp, 0, 0), slots), slots);
}

TEST(GlyphStoreFormat, HeaderLayoutIsStable) {
  // Bump kFormatVersion when any of these change.
  EXPECT_EQ(648u, sizeof(Header));
  EXPECT_EQ(644u, offsetof(Header, check));
  EXPECT_EQ(8u, sizeof(Slot));
  EXPECT_EQ(8u, sizeof(SizeEntry));
}

TEST(GlyphStoreFormat, ARecordIsItsHeaderAndBitmapRoundedToFourBytes) {
  EXPECT_EQ(20u, recordBytes(0));  // a space: the header alone
  EXPECT_EQ(24u, recordBytes(1));
  EXPECT_EQ(24u, recordBytes(4));
  EXPECT_EQ(28u, recordBytes(5));
}

TEST(GlyphStorePlan, LatinFlashModeFillsThePartition) {
  // Bookerly-sized: four styles of 450 KB, 8 pages each.
  Layout layout{};
  ASSERT_TRUE(planLayout(input(kModeFlash, 4, 460800), layout));
  const Geometry& g = layout.geometry;
  EXPECT_EQ(65536u, layout.styleOffset[0]);
  EXPECT_EQ(65536u + 3 * 8 * 65536u, layout.styleOffset[3]);
  EXPECT_EQ(33 * 65536u, g.indexOffset);
  EXPECT_EQ(2 * kSlotsPerPage, g.indexSlots);
  EXPECT_EQ(g.indexOffset + 2 * 65536u, g.pairOffset);
  EXPECT_EQ(kSlotsPerPage, g.pairSlots);
  EXPECT_EQ(g.pairOffset + 65536u, g.logOffset);
  EXPECT_EQ(17 * 65536u, g.logSize);
  EXPECT_EQ(kX3UsableBytes, g.logOffset + g.logSize);  // the partition, not the budget, was the limit
}

TEST(GlyphStorePlan, CjkSdModeGetsTheLargestLog) {
  // One style whose copied tables take 7 pages. They stay mapped, so they count against the budget.
  Layout layout{};
  ASSERT_TRUE(planLayout(input(kModeSd, 1, 430000), layout));
  const Geometry& g = layout.geometry;
  EXPECT_EQ(kModeSd, g.mode);
  EXPECT_EQ(8 * 65536u, g.indexOffset);
  EXPECT_EQ(kSlotsPerPage, g.indexSlots);
  EXPECT_EQ(21 * 65536u, g.logSize);
}

TEST(GlyphStorePlan, LargeStylesShrinkTheLog) {
  // Noto Sans-sized: four styles of 629 KB, 10 pages each.
  Layout layout{};
  ASSERT_TRUE(planLayout(input(kModeFlash, 4, 644096), layout));
  EXPECT_EQ(9 * 65536u, layout.geometry.logSize);
  EXPECT_EQ(2 * kSlotsPerPage, layout.geometry.indexSlots);
}

TEST(GlyphStorePlan, StylesThatDoNotFitAreRefused) {
  Layout layout{};
  EXPECT_FALSE(planLayout(input(kModeFlash, 4, 900000), layout));
}

TEST(GlyphStorePlan, ATightMappingBudgetIsRefused) {
  Layout layout{};
  EXPECT_FALSE(planLayout(input(kModeFlash, 4, 460800, 12), layout));
}

TEST(GlyphStorePlan, TheIndexHoldsWhatTheLogHolds) {
  // The index reaches 70 % no earlier than the log fills with typical records, so the store is not
  // wiped (and erased) while half the log is still empty.
  for (const uint32_t bytes : {200000u, 460800u, 644096u}) {
    Layout layout{};
    ASSERT_TRUE(planLayout(input(kModeFlash, 4, bytes), layout));
    const Geometry& g = layout.geometry;
    EXPECT_GE(uint64_t{g.indexSlots} * kIndexLoadPercent / 100, uint64_t{g.logSize} / kExpectedRecordFlash) << bytes;
  }
}

TEST(GlyphStorePlan, BadInputIsRefused) {
  Layout layout{};
  EXPECT_FALSE(planLayout(input(kModeFlash, 0, 1000), layout));
  EXPECT_FALSE(planLayout(input(kModeFlash, 5, 1000), layout));
  EXPECT_FALSE(planLayout(input(2, 1, 1000), layout));
  PlanInput empty = input(kModeFlash, 2, 1000);
  empty.styleBytes[1] = 0;
  EXPECT_FALSE(planLayout(empty, layout));
}

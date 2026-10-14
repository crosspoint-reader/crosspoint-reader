// The size table, kerning pairs and the wipe (spec 4.2, 4.3, 4.6, 4.7, 7.5).
#include <gtest/gtest.h>

#include <cstdint>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;
using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

class GlyphStoreSizes : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(installAndOpen(store, smallLayout())); }
};

class GlyphStorePairs : public GlyphStoreSizes {};

class GlyphStoreWipe : public GlyphStoreSizes {
 protected:
  // Claims all 16 sizes and one more: the store is then full, with wipe pending.
  void fillSizeTable() {
    for (uint32_t i = 0; i < kSizeEntries; ++i) ASSERT_EQ(static_cast<int>(i), store.sizeIndex((20 + i) << 6, 0));
    ASSERT_EQ(-1, store.sizeIndex(99 << 6, 0));
    ASSERT_TRUE(store.wipePending());
  }

  alignas(4) uint8_t buffer[4096] = {};
  GlyphBatch batch{buffer, sizeof buffer};
};

}  // namespace

TEST_F(GlyphStoreSizes, TheFirstSizeGetsIndexZeroAndKeepsIt) {
  EXPECT_EQ(0, store.sizeIndex(29 << 6, 0));
  EXPECT_EQ(1, store.sizeIndex(54 << 6, 1));
  EXPECT_EQ(0, store.sizeIndex(29 << 6, 0));
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1, store.sizeIndex(54 << 6, 1));
  EXPECT_EQ(2, store.sizeIndex(33 << 6, 0));
}

TEST_F(GlyphStoreSizes, TheSeventeenthSizeFillsTheStore) {
  for (uint32_t i = 0; i < kSizeEntries; ++i) ASSERT_EQ(static_cast<int>(i), store.sizeIndex((20 + i) << 6, 0));
  EXPECT_EQ(-1, store.sizeIndex(99 << 6, 0));
  EXPECT_TRUE(store.wipePending());
  EXPECT_EQ(3, store.sizeIndex(23 << 6, 0));  // known sizes are still answered
}

TEST_F(GlyphStoreSizes, ASizeEntryCutShortIsNeverReused) {
  flash.cutAfterSteps(5);  // the ppem and the flags byte land; the pad does not
  EXPECT_EQ(-1, store.sizeIndex(29 << 6, 0));
  flash.powerCycle();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1, store.sizeIndex(29 << 6, 0));  // entry 0 is spent, not programmed again
}

TEST_F(GlyphStorePairs, PairsAreStoredPerStyle) {
  ASSERT_TRUE(store.storePair('A', 'V', 0, -74));
  ASSERT_TRUE(store.storePair('A', 'V', 1, -60));
  int16_t kern = 0;
  ASSERT_TRUE(store.findPair('A', 'V', 0, &kern));
  EXPECT_EQ(-74, kern);
  ASSERT_TRUE(store.findPair('A', 'V', 1, &kern));
  EXPECT_EQ(-60, kern);
  EXPECT_FALSE(store.findPair('A', 'V', 2, &kern));
  EXPECT_FALSE(store.findPair('V', 'A', 0, &kern));
  EXPECT_EQ(2u, store.usedPairSlots());
}

TEST_F(GlyphStorePairs, ZeroIsAStoredAnswer) {
  // "No kerning" is worth storing as well: the GPOS reader is not asked again.
  ASSERT_TRUE(store.storePair('o', 'o', 0, 0));
  int16_t kern = 99;
  ASSERT_TRUE(store.findPair('o', 'o', 0, &kern));
  EXPECT_EQ(0, kern);
}

TEST_F(GlyphStorePairs, PairsSurviveAReopen) {
  for (uint16_t right = 'a'; right <= 'z'; ++right) {
    ASSERT_TRUE(store.storePair('T', right, 0, static_cast<int16_t>('a' - right)));
  }
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(26u, store.usedPairSlots());
  for (uint16_t right = 'a'; right <= 'z'; ++right) {
    int16_t kern = 1;
    ASSERT_TRUE(store.findPair('T', right, 0, &kern));
    EXPECT_EQ('a' - right, kern);
  }
}

TEST_F(GlyphStorePairs, ImpossiblePairsAreRefused) {
  EXPECT_FALSE(store.storePair(0xFFFF, 0xFFFF, 0, 1));  // would read as an erased slot
  EXPECT_FALSE(store.storePair('A', 'V', 4, 1));        // styles are 0-3
  EXPECT_EQ(0u, store.usedPairSlots());
}

TEST(GlyphStorePairsFull, AFullPairIndexStopsStoringButNeverWipes) {
  SimNorFlash flash(kFlashBytes);
  FlashGlyphStore store(flash);
  ASSERT_TRUE(installAndOpen(store, smallLayout(1024, 100)));
  uint32_t stored = 0;
  for (uint16_t right = 0x100; right < 0x200 && store.storePair(0x41, right, 0, -1); ++right) ++stored;
  EXPECT_EQ(70u, stored);
  EXPECT_FALSE(store.wipePending());
  alignas(4) uint8_t buffer[256];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  EXPECT_EQ(Commit::Stored, store.commit(batch));  // glyphs are not affected
  EXPECT_EQ(0u, flash.violations());
}

TEST_F(GlyphStoreWipe, WipeEmptiesTheStoreAndKeepsTheFamily) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  ASSERT_EQ(Commit::Stored, store.markMissing(makeKey(0x2603, 0, 0)));
  ASSERT_TRUE(store.storePair('A', 'V', 0, -74));
  fillSizeTable();
  ASSERT_TRUE(store.wipe());
  for (int pass = 0; pass < 2; ++pass) {  // as wiped, then again after a reopen
    EXPECT_FALSE(store.wipePending()) << pass;
    EXPECT_EQ(Lookup::Absent, store.find(makeKey('A', 0, 0), nullptr)) << pass;
    EXPECT_EQ(Lookup::Absent, store.find(makeKey(0x2603, 0, 0), nullptr)) << pass;
    EXPECT_FALSE(store.findPair('A', 'V', 0, nullptr)) << pass;
    EXPECT_EQ(0u, store.logUsed()) << pass;
    EXPECT_EQ(0u, store.usedSlots()) << pass;
    EXPECT_EQ(0u, store.usedPairSlots()) << pass;
    const uint8_t* style = store.mapStyle(0);  // the font itself is kept
    ASSERT_NE(nullptr, style);
    EXPECT_EQ(styleByte(0, 7), style[7]);
    store.close();
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  }
  EXPECT_EQ(0, store.sizeIndex(54 << 6, 0));  // the size table starts over
  ASSERT_TRUE(stage(batch, makeKey('B', 0, 0), 9, 12));
  EXPECT_EQ(Commit::Stored, store.commit(batch));
}

TEST_F(GlyphStoreWipe, WipeErasesOnlySectorsThatHoldData) {
  for (uint32_t cp = 'a'; cp < 'a' + 3; ++cp) ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  fillSizeTable();
  const Geometry g = store.geometry();
  const uint32_t before = flash.totalSectorErases();
  ASSERT_TRUE(store.wipe());
  // The header, the log's first sector and at most three index sectors; no pair sector (no pair stored).
  EXPECT_LE(flash.totalSectorErases() - before, 5u);
  EXPECT_EQ(1u, flash.sectorErases(g.logOffset / kSectorBytes));
  EXPECT_EQ(0u, flash.sectorErases(g.logOffset / kSectorBytes + 1));
  EXPECT_EQ(0u, flash.sectorErases(g.pairOffset / kSectorBytes));
}

TEST_F(GlyphStoreWipe, WipingAnEmptyStoreErasesOnlyTheHeader) {
  fillSizeTable();
  const uint32_t before = flash.totalSectorErases();
  ASSERT_TRUE(store.wipe());
  EXPECT_EQ(1u, flash.totalSectorErases() - before);
  EXPECT_EQ(1u, flash.sectorErases(0));
}

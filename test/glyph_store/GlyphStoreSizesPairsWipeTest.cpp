// The size table, kerning pairs and the wipe.
#include <gtest/gtest.h>

#include <cstdint>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

// A store with an empty size table.
class GlyphStoreSizes : public StoreTest {
 protected:
  void SetUp() override {
    ASSERT_TRUE(install(store, smallLayout()));
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  }

  int claim(const uint32_t ppem26_6, const uint8_t family = 0) {
    uint8_t index = 0;
    return store.faceIndex(family, ppem26_6, &index) ? index : -1;
  }
};

class GlyphStorePairs : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(installAndOpen(store, smallLayout())); }
};

class GlyphStoreWipe : public GlyphStorePairs {
 protected:
  alignas(4) uint8_t buffer[4096] = {};
  GlyphBatch batch{buffer, sizeof buffer};
};

}  // namespace

TEST_F(GlyphStoreSizes, TheFirstSizeGetsIndexZeroAndKeepsIt) {
  EXPECT_EQ(0, claim(29 << 6));
  EXPECT_EQ(1, claim(54 << 6));
  EXPECT_EQ(0, claim(29 << 6));
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1, claim(54 << 6));
  EXPECT_EQ(2, claim(33 << 6));
}

TEST_F(GlyphStoreSizes, WhenEverySizeIsTakenTheStoreIsFull) {
  for (uint32_t i = 0; i < kFaceEntries; ++i) ASSERT_EQ(static_cast<int>(i), claim((20 + i) << 6));
  EXPECT_EQ(-1, claim(99 << 6));
  EXPECT_TRUE(store.wipePending());
  EXPECT_EQ(3, claim(23 << 6));  // known sizes are still answered
}

TEST_F(GlyphStoreSizes, ClaimingTheLastFreeSizeMakesTheWipePending) {
  // Otherwise a table filled exactly at the end of one book refuses the next book's body size until a book after it.
  for (uint32_t i = 0; i + 1 < kFaceEntries; ++i) ASSERT_EQ(static_cast<int>(i), claim((20 + i) << 6));
  EXPECT_FALSE(store.wipePending());
  EXPECT_EQ(static_cast<int>(kFaceEntries - 1), claim(99 << 6));
  EXPECT_TRUE(store.wipePending());
  EXPECT_FALSE(store.writesStopped());
  EXPECT_EQ(3, claim(23 << 6));  // claimed sizes are still answered
}

TEST_F(GlyphStoreSizes, AFullSizeTableStopsOnlyNewSizes) {
  for (uint32_t i = 0; i < kFaceEntries; ++i) ASSERT_EQ(static_cast<int>(i), claim((20 + i) << 6));
  EXPECT_EQ(-1, claim(99 << 6));
  ASSERT_TRUE(store.wipePending());
  EXPECT_FALSE(store.writesStopped());
  // The claimed sizes still store their glyphs, missing glyphs and pairs until the wipe.
  alignas(4) uint8_t buffer[256];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 5), 9, 12));
  EXPECT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_EQ(Lookup::Found, store.find(makeKey('A', 0, 5), nullptr));
  EXPECT_EQ(Commit::Stored, store.markNoGlyph(makeKey(0x2603, 0, 5)));
  EXPECT_TRUE(store.storePair('A', 'V', 0, 0, -74));
  // The next session wipes first.
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.wipePending());
  EXPECT_TRUE(store.writesStopped());
}

TEST_F(GlyphStoreSizes, EachFamilyOfTheSetHasItsOwnFaces) {
  EXPECT_EQ(0, claim(29 << 6));
  EXPECT_EQ(1, claim(29 << 6, 1));  // the same size in the accent family is another face
  EXPECT_EQ(2, claim(35 << 6, 1));
  EXPECT_EQ(0, claim(29 << 6));
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1, claim(29 << 6, 1));
  EXPECT_EQ(2, claim(35 << 6, 1));
  EXPECT_EQ(3, claim(35 << 6));
  EXPECT_EQ(-1, claim(29 << 6, kMaxStyles));  // no set has that many families
}

TEST_F(GlyphStoreSizes, TheFamiliesShareTheTable) {
  // A wipe at the next book open frees it, as for one family.
  for (uint32_t i = 0; i < kFaceEntries; ++i) {
    ASSERT_EQ(static_cast<int>(i), claim((20 + i / 2) << 6, static_cast<uint8_t>(i % 2)));
  }
  EXPECT_TRUE(store.wipePending());
  EXPECT_EQ(-1, claim(99 << 6, 1));
  EXPECT_EQ(5, claim(22 << 6, 1));  // claimed faces are still answered
}

TEST_F(GlyphStoreSizes, TwoFamiliesGlyphsAreApart) {
  const int body = claim(29 << 6);
  const int accent = claim(29 << 6, 1);
  ASSERT_GE(body, 0);
  ASSERT_GE(accent, 0);
  alignas(4) uint8_t buffer[512];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(stage(batch, makeKey('A', 0, static_cast<uint8_t>(body)), 9, 12));
  ASSERT_TRUE(stage(batch, makeKey('A', 0, static_cast<uint8_t>(accent)), 11, 13));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_TRUE(storedIntact(store, makeKey('A', 0, static_cast<uint8_t>(body)), 9, 12));
  EXPECT_TRUE(storedIntact(store, makeKey('A', 0, static_cast<uint8_t>(accent)), 11, 13));
}

TEST_F(GlyphStoreSizes, ASizeEntryCutShortIsNeverReused) {
  flash.cutAfterSteps(5);  // the ppem and one byte of `claimed` land
  EXPECT_EQ(-1, claim(29 << 6));
  flash.powerCycle();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1, claim(29 << 6));  // entry 0 is spent, not programmed again
}

TEST_F(GlyphStoreSizes, AnEntryWhoseClaimLandedWithoutItsPpemIsNeverReused) {
  // On a real chip the bits of an 8-byte write can land in any order.
  const uint32_t claimed = 0;
  ASSERT_TRUE(flash.program(kFaceTableOffset + 4, &claimed, sizeof claimed));
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1, claim(29 << 6));
}

TEST_F(GlyphStorePairs, PairsAreStoredPerStyle) {
  ASSERT_TRUE(store.storePair('A', 'V', 0, 0, -74));
  ASSERT_TRUE(store.storePair('A', 'V', 0, 1, -60));
  int16_t kern = 0;
  ASSERT_TRUE(store.findPair('A', 'V', 0, 0, &kern));
  EXPECT_EQ(-74, kern);
  ASSERT_TRUE(store.findPair('A', 'V', 0, 1, &kern));
  EXPECT_EQ(-60, kern);
  EXPECT_FALSE(store.findPair('A', 'V', 0, 2, &kern));
  EXPECT_FALSE(store.findPair('V', 'A', 0, 0, &kern));
  EXPECT_EQ(2u, store.usedPairSlots());
}

TEST_F(GlyphStorePairs, PairsAreStoredPerFamily) {
  ASSERT_TRUE(store.storePair('A', 'V', 0, 0, -74));
  ASSERT_TRUE(store.storePair('A', 'V', 1, 0, -31));
  ASSERT_TRUE(store.storePair('A', 'V', 1, 1, -40));
  int16_t kern = 0;
  ASSERT_TRUE(store.findPair('A', 'V', 0, 0, &kern));
  EXPECT_EQ(-74, kern);
  ASSERT_TRUE(store.findPair('A', 'V', 1, 0, &kern));
  EXPECT_EQ(-31, kern);
  ASSERT_TRUE(store.findPair('A', 'V', 1, 1, &kern));
  EXPECT_EQ(-40, kern);
  EXPECT_FALSE(store.findPair('A', 'V', 2, 0, &kern));
  EXPECT_FALSE(store.findPair('A', 'V', 0, 1, &kern));
  EXPECT_EQ(3u, store.usedPairSlots());
}

TEST_F(GlyphStorePairs, ZeroIsAStoredAnswer) {
  // "No kerning" is worth storing as well: the GPOS reader is not asked again.
  ASSERT_TRUE(store.storePair('o', 'o', 0, 0, 0));
  int16_t kern = 99;
  ASSERT_TRUE(store.findPair('o', 'o', 0, 0, &kern));
  EXPECT_EQ(0, kern);
}

TEST_F(GlyphStorePairs, PairsSurviveAReopen) {
  for (uint16_t right = 'a'; right <= 'z'; ++right) {
    ASSERT_TRUE(store.storePair('T', right, 0, 0, static_cast<int16_t>('a' - right)));
  }
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(26u, store.usedPairSlots());
  for (uint16_t right = 'a'; right <= 'z'; ++right) {
    int16_t kern = 1;
    ASSERT_TRUE(store.findPair('T', right, 0, 0, &kern));
    EXPECT_EQ('a' - right, kern);
  }
}

TEST_F(GlyphStorePairs, ImpossiblePairsAreRefused) {
  EXPECT_FALSE(store.storePair(0xFFFF, 0xFFFF, 0, 0, 1));     // would read as an erased slot
  EXPECT_FALSE(store.storePair('A', 'V', 0, 4, 1));           // styles are 0-3
  EXPECT_FALSE(store.storePair('A', 'V', kMaxStyles, 0, 1));  // no set has that many families
  EXPECT_EQ(0u, store.usedPairSlots());
}

TEST_F(GlyphStorePairs, PairsAreNotStoredWhileWritesAreStopped) {
  fillSizeTable(store);
  store.close();  // a store that opens with wipe pending writes nothing until the wipe
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_TRUE(store.writesStopped());
  EXPECT_FALSE(store.storePair('A', 'V', 0, 0, -74));
  EXPECT_EQ(0u, store.usedPairSlots());
}

TEST_F(GlyphStorePairs, AFullPairIndexStopsStoringButNeverWipes) {
  store.close();
  ASSERT_TRUE(installAndOpen(store, smallLayout(1024, 200)));
  uint32_t stored = 0;
  for (uint16_t right = 0x100; right < 0x300 && store.storePair(0x41, right, 0, 0, -1); ++right) ++stored;
  EXPECT_EQ(140u, stored);
  EXPECT_FALSE(store.wipePending());
  alignas(4) uint8_t buffer[256];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  EXPECT_EQ(Commit::Stored, store.commit(batch));  // glyphs are not affected
}

TEST_F(GlyphStoreWipe, WipeEmptiesTheStoreAndKeepsTheFamily) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  ASSERT_EQ(Commit::Stored, store.markNoGlyph(makeKey(0x2603, 0, 0)));
  ASSERT_TRUE(store.storePair('A', 'V', 0, 0, -74));
  fillSizeTable(store);
  ASSERT_TRUE(store.wipe());
  for (int pass = 0; pass < 2; ++pass) {  // as wiped, then again after a reopen
    EXPECT_FALSE(store.wipePending()) << pass;
    EXPECT_EQ(Lookup::NotStored, store.find(makeKey('A', 0, 0), nullptr)) << pass;
    EXPECT_EQ(Lookup::NotStored, store.find(makeKey(0x2603, 0, 0), nullptr)) << pass;
    EXPECT_FALSE(store.findPair('A', 'V', 0, 0, nullptr)) << pass;
    EXPECT_EQ(0u, store.logUsed()) << pass;
    EXPECT_EQ(0u, store.usedSlots()) << pass;
    EXPECT_EQ(0u, store.usedPairSlots()) << pass;
    const uint8_t* style = store.mapStyle(0);  // the font itself is kept
    ASSERT_NE(nullptr, style);
    EXPECT_EQ(styleByte(0, 7), style[7]);
    store.close();
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  }
  ASSERT_TRUE(claimTestSizes(store));  // the size table starts over
  ASSERT_TRUE(stage(batch, makeKey('B', 0, 0), 9, 12));
  EXPECT_EQ(Commit::Stored, store.commit(batch));
}

TEST_F(GlyphStoreWipe, AWipeVoidsTheSizeIndicesClaimedBeforeIt) {
  const uint32_t generation = store.generation();
  fillSizeTable(store);
  ASSERT_TRUE(store.wipe());
  EXPECT_NE(generation, store.generation());
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 2), 9, 12));
  EXPECT_EQ(Commit::Refused, store.commit(batch));  // size index 2 is not claimed any more
  uint8_t index = 0xFF;
  ASSERT_TRUE(store.faceIndex(0, 54 << 6, &index));  // another size takes entry 0 now
  EXPECT_EQ(0, index);
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 2), 9, 12));
  EXPECT_EQ(Commit::Refused, store.commit(batch));
}

TEST_F(GlyphStoreWipe, AWipeOfAStoreNotFullWorksToo) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  ASSERT_FALSE(store.wipePending());
  ASSERT_TRUE(store.wipe());
  EXPECT_FALSE(store.wipePending());
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey('A', 0, 0), nullptr));
}

TEST_F(GlyphStoreWipe, WipeErasesOnlySectorsThatHoldData) {
  for (uint32_t cp = 'a'; cp < 'a' + 3; ++cp) ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  fillSizeTable(store);
  const Geometry g = store.geometry();
  const uint32_t before = flash.totalSectorErases();
  ASSERT_TRUE(store.wipe());
  // The state sector, the log's first sector and at most two index sectors; no pair sector (no pair stored),
  // and never the header.
  EXPECT_LE(flash.totalSectorErases() - before, 4u);
  EXPECT_EQ(1u, flash.sectorErases(g.logOffset / kSectorBytes));
  EXPECT_EQ(0u, flash.sectorErases(g.logOffset / kSectorBytes + 1));
  EXPECT_EQ(0u, flash.sectorErases(g.pairOffset / kSectorBytes));
  EXPECT_EQ(0u, flash.sectorErases(0));
}

TEST_F(GlyphStoreWipe, WipingAnEmptyStoreErasesOnlyTheStateSector) {
  fillSizeTable(store);
  const uint32_t before = flash.totalSectorErases();
  ASSERT_TRUE(store.wipe());
  EXPECT_EQ(1u, flash.totalSectorErases() - before);
  EXPECT_EQ(1u, flash.sectorErases(kStateSectorOffset / kSectorBytes));
}

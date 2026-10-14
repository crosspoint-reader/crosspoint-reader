// Storing and finding rendered glyphs, and when the store counts as full.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

class GlyphStoreGlyphs : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(installAndOpen(store, layout)); }

  void reinstall(const Layout& other) {
    store.close();
    layout = other;
    ASSERT_TRUE(installAndOpen(store, layout));
  }

  Layout layout = smallLayout();
  alignas(4) uint8_t buffer[16384] = {};
  GlyphBatch batch{buffer, sizeof buffer};
};

// Keys with the same home slot, found by search: a probe-chain test needs real collisions.
std::vector<uint32_t> collidingKeys(const uint32_t slots, const size_t count) {
  std::vector<uint32_t> keys;
  const uint32_t home = homeSlot(makeKey(0x4E00, 0, 0), slots);
  for (uint32_t cp = 0x4E00; keys.size() < count && cp < 0x10FFFF; ++cp) {
    if (homeSlot(makeKey(cp, 0, 0), slots) == home) keys.push_back(makeKey(cp, 0, 0));
  }
  return keys;
}

uint8_t widthOf(const uint32_t cp) { return static_cast<uint8_t>(6 + cp % 9); }
uint8_t heightOf(const uint32_t cp) { return static_cast<uint8_t>(9 + cp % 7); }

}  // namespace

TEST_F(GlyphStoreGlyphs, ABatchIsStoredAndFound) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_TRUE(stage(batch, makeKey('g', 1, 0), 8, 14));
  ASSERT_TRUE(stage(batch, makeKey(0x4E2D, 0, 2), 24, 26));
  EXPECT_EQ(3, batch.count());
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_EQ(0, batch.count());  // ready to be filled again
  EXPECT_TRUE(storedIntact(store, makeKey('A', 0, 0), 9, 12));
  EXPECT_TRUE(storedIntact(store, makeKey('g', 1, 0), 8, 14));
  EXPECT_TRUE(storedIntact(store, makeKey(0x4E2D, 0, 2), 24, 26));
  EXPECT_EQ(3u, store.usedSlots());
}

TEST_F(GlyphStoreGlyphs, ABatchIsOneLogWritePlusOneSlotWritePerGlyph) {
  for (uint32_t cp = 'a'; cp < 'a' + 10; ++cp) ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 9, 12));
  const uint32_t before = flash.programCalls();
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_EQ(before + 1 + 10, flash.programCalls());
}

TEST_F(GlyphStoreGlyphs, RecordsAreLaidOutAsTheFormatSays) {
  const uint32_t first = makeKey('A', 0, 0);
  ASSERT_TRUE(stage(batch, first, 9, 12));
  ASSERT_TRUE(stage(batch, makeKey('B', 0, 0), 10, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  const uint8_t* log = flash.raw() + layout.geometry.logOffset;
  uint32_t key = 0;
  std::memcpy(&key, log, sizeof key);
  EXPECT_EQ(first, key);
  const EpdGlyph* found = nullptr;
  ASSERT_EQ(Lookup::Found, store.find(first, &found));
  EXPECT_EQ(store.logBase() + 4, reinterpret_cast<const uint8_t*>(found));  // the EpdGlyph sits at offset 4
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(found) % 4);
  EXPECT_EQ(kRecordHeaderBytes, found->dataOffset);  // relative to the log base
  std::memcpy(&key, log + recordSize(9, 12), sizeof key);
  EXPECT_EQ(makeKey('B', 0, 0), key);  // the next record starts 4-byte aligned
  EXPECT_EQ(recordSize(9, 12) + recordSize(10, 12), store.logUsed());
}

TEST_F(GlyphStoreGlyphs, AGlyphWithNoPixelsIsStored) {
  ASSERT_TRUE(stage(batch, makeKey(' ', 0, 0), 0, 0));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_TRUE(storedIntact(store, makeKey(' ', 0, 0), 0, 0));
}

TEST_F(GlyphStoreGlyphs, AnUnknownKeyIsNotStored) {
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey('Q', 0, 0), nullptr));
  EXPECT_EQ(Lookup::NotStored, store.find(kErased, nullptr));
}

TEST_F(GlyphStoreGlyphs, GlyphsTheFontLacksAreRemembered) {
  const uint32_t key = makeKey(0x2603, 0, 0);
  ASSERT_EQ(Commit::Stored, store.markNoGlyph(key));
  EXPECT_EQ(Lookup::NoGlyph, store.find(key, nullptr));
  EXPECT_EQ(0u, store.logUsed());  // such a key has no record
  EXPECT_EQ(1u, store.usedSlots());
}

TEST_F(GlyphStoreGlyphs, EverythingSurvivesAReopen) {
  for (uint32_t cp = 0x21; cp < 0x21 + 50; ++cp) {
    ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), widthOf(cp), heightOf(cp)));
    if (cp % 10 == 0) {
      ASSERT_EQ(Commit::Stored, store.commit(batch));
    }
  }
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  for (uint32_t cp = 0x3000; cp < 0x3005; ++cp) ASSERT_EQ(Commit::Stored, store.markNoGlyph(makeKey(cp, 0, 0)));
  const uint32_t logUsed = store.logUsed();
  const uint32_t slots = store.usedSlots();
  store.close();
  FlashGlyphStore reopened(flash);
  ASSERT_EQ(OpenResult::Ready, reopened.open(kIdentityA));
  EXPECT_EQ(logUsed, reopened.logUsed());
  EXPECT_EQ(slots, reopened.usedSlots());
  for (uint32_t cp = 0x21; cp < 0x21 + 50; ++cp) {
    EXPECT_TRUE(storedIntact(reopened, makeKey(cp, 0, 0), widthOf(cp), heightOf(cp))) << cp;
  }
  for (uint32_t cp = 0x3000; cp < 0x3005; ++cp) {
    EXPECT_EQ(Lookup::NoGlyph, reopened.find(makeKey(cp, 0, 0), nullptr)) << cp;
  }
}

TEST_F(GlyphStoreGlyphs, CollidingKeysProbeOn) {
  const std::vector<uint32_t> keys = collidingKeys(layout.geometry.indexSlots, 5);
  ASSERT_EQ(5u, keys.size());
  for (const uint32_t key : keys) ASSERT_TRUE(stage(batch, key, 7, 9));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  for (const uint32_t key : keys) EXPECT_TRUE(storedIntact(store, key, 7, 9));
}

TEST_F(GlyphStoreGlyphs, ALongProbeChainCountsAsFull) {
  const std::vector<uint32_t> keys = collidingKeys(layout.geometry.indexSlots, kMaxProbe + 1);
  ASSERT_EQ(kMaxProbe + 1, keys.size());
  for (uint32_t i = 0; i < kMaxProbe; ++i) ASSERT_EQ(Commit::Stored, store.markNoGlyph(keys[i])) << i;
  EXPECT_EQ(Commit::Full, store.markNoGlyph(keys[kMaxProbe]));
  EXPECT_TRUE(store.wipePending());
}

TEST_F(GlyphStoreGlyphs, SeventyPercentOfTheIndexCountsAsFull) {
  reinstall(smallLayout(1000));
  uint32_t stored = 0;
  for (uint32_t cp = 0x4E00; cp < 0x4E00 + 1000; ++cp) {
    const Commit result = store.markNoGlyph(makeKey(cp, 0, 0));
    if (result != Commit::Stored) {
      EXPECT_EQ(Commit::Full, result);
      break;
    }
    ++stored;
  }
  EXPECT_EQ(700u, stored);
  EXPECT_TRUE(store.wipePending());
}

TEST_F(GlyphStoreGlyphs, ANearlyFullLogCountsAsFull) {
  // A 64 KB log, filled with 2,020-byte records (100 x 80 bitmaps), one per batch.
  uint32_t stored = 0;
  for (uint32_t cp = 0x41;; ++cp) {
    ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 100, 80));
    const Commit result = store.commit(batch);
    if (result != Commit::Stored) {
      EXPECT_EQ(Commit::Full, result);
      break;
    }
    ++stored;
  }
  const uint64_t limit = uint64_t{layout.geometry.logSize} * kLogFullPercent;
  EXPECT_LE(uint64_t{store.logUsed()} * 100, limit);
  EXPECT_GT((uint64_t{store.logUsed()} + recordSize(100, 80)) * 100, limit);
  EXPECT_EQ(store.logUsed() / recordSize(100, 80), stored);
  EXPECT_TRUE(store.wipePending());
}

TEST_F(GlyphStoreGlyphs, MalformedRecordsAreRefusedAtStaging) {
  EXPECT_EQ(nullptr, batch.add(makeKey('W', 0, 0), metrics(255, 255)));  // 16,257 B, over the 8 KB cap
  EpdGlyph wrong = metrics(10, 10);
  wrong.dataLength = 7;  // not the size of a 10 x 10 bitmap: the scan would take it for a torn record
  EXPECT_EQ(nullptr, batch.add(makeKey('X', 0, 0), wrong));
  EXPECT_EQ(nullptr, batch.add(0x10000000u | 'Y', metrics(9, 12)));      // not a glyph key: it would end the scan
  EXPECT_NE(nullptr, batch.add(makeKey('V', 0, 0), metrics(181, 181)));  // 8,191 B
  EXPECT_EQ(1, batch.count());
}

TEST_F(GlyphStoreGlyphs, ABatchThatDoesNotFitTheBufferIsRefused) {
  uint8_t small[64];
  GlyphBatch tiny(small, sizeof small);
  EXPECT_EQ(nullptr, tiny.add(makeKey('M', 0, 0), metrics(30, 30)));  // 248 B with its header
  EXPECT_NE(nullptr, tiny.add(makeKey('i', 0, 0), metrics(4, 12)));   // 32 B
  EXPECT_EQ(1, tiny.count());
}

TEST_F(GlyphStoreGlyphs, KeysOfAnUnclaimedSizeAreRefused) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_TRUE(stage(batch, makeKey('B', 0, kTestSizes), 9, 12));  // size index kTestSizes was never claimed
  const uint32_t calls = flash.programCalls();
  EXPECT_EQ(Commit::Refused, store.commit(batch));
  EXPECT_EQ(Commit::Refused, store.markNoGlyph(makeKey('C', 0, kTestSizes)));
  EXPECT_EQ(calls, flash.programCalls());  // nothing written...
  EXPECT_FALSE(store.wipePending());       // ...and the store is not full
  EXPECT_EQ(0, batch.count());
}

TEST_F(GlyphStoreGlyphs, AClosedStoreRefusesWrites) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  store.close();
  EXPECT_EQ(Commit::Refused, store.commit(batch));
  EXPECT_EQ(Commit::Refused, store.markNoGlyph(makeKey('B', 0, 0)));
}

TEST_F(GlyphStoreGlyphs, AFailedLogWriteSetsWipePending) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  flash.failNextProgram();
  EXPECT_EQ(Commit::WriteFailed, store.commit(batch));
  EXPECT_TRUE(store.wipePending());
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.wipePending());  // recorded in flash, not only for this session
}

TEST_F(GlyphStoreGlyphs, AFailedSlotWriteSetsWipePending) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  flash.failNextProgram(1);  // the record lands; its slot does not
  EXPECT_EQ(Commit::WriteFailed, store.commit(batch));
  EXPECT_TRUE(store.wipePending());
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey('A', 0, 0), nullptr));
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.wipePending());
}

TEST_F(GlyphStoreGlyphs, StoredGlyphsStayReadableWhileWipeIsPending) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  ASSERT_TRUE(stage(batch, makeKey('B', 0, 0), 9, 12));
  flash.failNextProgram();
  ASSERT_EQ(Commit::WriteFailed, store.commit(batch));
  ASSERT_TRUE(stage(batch, makeKey('C', 0, 0), 9, 12));
  EXPECT_EQ(Commit::Full, store.commit(batch));  // nothing new is stored...
  EXPECT_EQ(Commit::Full, store.markNoGlyph(makeKey('D', 0, 0)));
  EXPECT_TRUE(storedIntact(store, makeKey('A', 0, 0), 9, 12));  // ...but what is stored is still served
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey('B', 0, 0), nullptr));
}

TEST_F(GlyphStoreGlyphs, ReinstallingErasesOnlySectorsThatHoldData) {
  for (uint32_t cp = 'a'; cp < 'a' + 3; ++cp) ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  store.close();
  const uint32_t before = flash.totalSectorErases();
  ASSERT_TRUE(install(store, layout, kIdentityB));
  // The header, the state sector, the style, the log's first sector, and at most two index sectors.
  EXPECT_LE(flash.totalSectorErases() - before, 6u);
  const uint32_t logSector = layout.geometry.logOffset / kSectorBytes;
  EXPECT_EQ(1u, flash.sectorErases(logSector));
  EXPECT_EQ(0u, flash.sectorErases(logSector + 1));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityB));
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey('a', 0, 0), nullptr));
}

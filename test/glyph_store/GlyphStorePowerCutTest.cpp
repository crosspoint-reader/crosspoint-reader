// A power cut at every step of every write. Whatever the store reported as stored is intact after the
// reboot, nothing is served torn, and writing carries on without programming any byte twice.
#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <string>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

constexpr uint32_t kBodyPpem = 29 << 6;
constexpr uint32_t kHeadingPpem = 35 << 6;

// The size indices a session claimed; a torn claim makes the next one land elsewhere, so keys are built
// from what sizeIndex() returned, not assumed.
struct Sizes {
  uint8_t body = 0xFF;
  uint8_t heading = 0xFF;
};

struct GlyphSpec {
  uint32_t codepoint;
  uint8_t style;
  bool heading;
  uint8_t width;
  uint8_t height;

  uint32_t key(const Sizes& sizes) const { return makeKey(codepoint, style, heading ? sizes.heading : sizes.body); }
};

// 'B' is 16 x 24 so that a record lands at an offset whose slot value, cut short, would once have read as the
// "no glyph" marker.
const GlyphSpec kBatchA[] = {{'A', 0, false, 9, 12}, {'B', 0, false, 16, 24}, {' ', 0, false, 0, 0}};
const GlyphSpec kBatchB[] = {{'C', 0, false, 11, 13}, {'d', 1, false, 8, 12}, {0x4E2D, 0, true, 24, 26}};
const GlyphSpec kAfter[] = {{'E', 0, false, 9, 13}, {'F', 0, false, 9, 12}};
constexpr uint32_t kNoGlyphCodepoint = 0x2603;

// What each step of a session reported.
struct Outcome {
  bool installed = false;
  Sizes sizes;
  bool batchA = false;
  bool noGlyph = false;
  bool batchB = false;
  bool pair = false;
};

Layout sweepLayout() {
  Layout layout = smallLayout();
  layout.styleSize[0] = 256;  // a short style keeps the sweep fast
  return layout;
}

bool claim(FlashGlyphStore& store, Sizes& sizes) {
  return store.sizeIndex(kBodyPpem, &sizes.body) && store.sizeIndex(kHeadingPpem, &sizes.heading);
}

bool commitAll(FlashGlyphStore& store, const GlyphSpec* glyphs, const size_t count, const Sizes& sizes) {
  alignas(4) uint8_t buffer[2048];
  GlyphBatch batch(buffer, sizeof buffer);
  for (size_t i = 0; i < count; ++i) {
    if (!stage(batch, glyphs[i].key(sizes), glyphs[i].width, glyphs[i].height)) return false;
  }
  return store.commit(batch) == Commit::Stored;
}

// Every kind of write a session makes, from the install on.
Outcome runSession(SimNorFlash& flash) {
  Outcome out;
  FlashGlyphStore store(flash);
  out.installed = install(store, sweepLayout());
  if (!out.installed || store.open(kIdentityA) != OpenResult::Ready || !claim(store, out.sizes)) return out;
  out.batchA = commitAll(store, kBatchA, std::size(kBatchA), out.sizes);
  out.noGlyph = store.markNoGlyph(makeKey(kNoGlyphCodepoint, 0, out.sizes.body)) == Commit::Stored;
  out.batchB = commitAll(store, kBatchB, std::size(kBatchB), out.sizes);
  out.pair = store.storePair('A', 'V', 0, -74);
  return out;
}

void checkGlyphs(const FlashGlyphStore& store, const GlyphSpec* glyphs, const size_t count, const Sizes& sizes,
                 const bool reported, const std::string& where) {
  for (size_t i = 0; i < count; ++i) {
    const GlyphSpec& g = glyphs[i];
    if (reported) {
      EXPECT_TRUE(storedIntact(store, g.key(sizes), g.width, g.height)) << where << " U+" << std::hex << g.codepoint;
    } else {
      EXPECT_TRUE(absentOrIntact(store, g.key(sizes), g.width, g.height)) << where << " U+" << std::hex << g.codepoint;
    }
  }
}

void checkAfterReboot(SimNorFlash& flash, const Outcome& out, const std::string& where) {
  FlashGlyphStore store(flash);
  const OpenResult opened = store.open(kIdentityA);
  if (out.installed) {
    ASSERT_EQ(OpenResult::Ready, opened) << where;
  } else if (opened != OpenResult::Ready) {
    EXPECT_EQ(OpenResult::NotInstalled, opened) << where;
    return;  // the next install starts from scratch
  }
  // A size claimed in the session answers the same now; a torn claim lands on a fresh entry instead.
  Sizes sizes;
  ASSERT_TRUE(claim(store, sizes)) << where;
  if (out.sizes.body != 0xFF) {
    EXPECT_EQ(out.sizes.body, sizes.body) << where;
  }
  if (out.sizes.heading != 0xFF) {
    EXPECT_EQ(out.sizes.heading, sizes.heading) << where;
  }
  checkGlyphs(store, kBatchA, std::size(kBatchA), sizes, out.batchA, where);
  checkGlyphs(store, kBatchB, std::size(kBatchB), sizes, out.batchB, where);
  const Lookup noGlyph = store.find(makeKey(kNoGlyphCodepoint, 0, sizes.body), nullptr);
  if (out.noGlyph) {
    EXPECT_EQ(Lookup::NoGlyph, noGlyph) << where;
  } else {
    EXPECT_NE(Lookup::Found, noGlyph) << where;
  }
  int16_t kern = 0;
  if (store.findPair('A', 'V', 0, &kern)) {
    EXPECT_EQ(-74, kern) << where;
  } else {
    EXPECT_FALSE(out.pair) << where;
  }
  // No glyph of the session reads as "the font has none" unless it was marked so.
  for (const GlyphSpec& g : kBatchA) EXPECT_NE(Lookup::NoGlyph, store.find(g.key(sizes), nullptr)) << where;
  for (const GlyphSpec& g : kBatchB) EXPECT_NE(Lookup::NoGlyph, store.find(g.key(sizes), nullptr)) << where;
  // The store carries on: a new batch lands, and everything is still there after another reboot.
  ASSERT_FALSE(store.wipePending()) << where;
  ASSERT_TRUE(commitAll(store, kAfter, std::size(kAfter), sizes)) << where;
  store.close();
  FlashGlyphStore again(flash);
  ASSERT_EQ(OpenResult::Ready, again.open(kIdentityA)) << where;
  checkGlyphs(again, kAfter, std::size(kAfter), sizes, true, where);
  EXPECT_EQ(0u, flash.violations()) << where;
  EXPECT_EQ(0u, flash.reprograms()) << where;
}

// 200 more glyphs, 12.8 KB of log over several sectors, so a cut can leave a wipe half done.
constexpr uint32_t kManyFirst = 0x4E00;
constexpr uint32_t kMany = 200;
constexpr Sizes kPopulateSizes{0, 1};  // what claim() returns on a fresh store

bool commitMany(FlashGlyphStore& store) {
  alignas(4) uint8_t buffer[4096];
  GlyphBatch batch(buffer, sizeof buffer);
  for (uint32_t i = 0; i < kMany; ++i) {
    const uint32_t key = makeKey(kManyFirst + i, 0, kPopulateSizes.body);
    if (stage(batch, key, 11, 16)) continue;
    if (store.commit(batch) != Commit::Stored || !stage(batch, key, 11, 16)) return false;
  }
  return store.commit(batch) == Commit::Stored;
}

// A store with data; with wipe pending as the full path leaves it, or not (a wipe asked for directly).
void populate(SimNorFlash& flash, const bool pending) {
  FlashGlyphStore store(flash);
  ASSERT_TRUE(install(store, sweepLayout()));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  Sizes sizes;
  ASSERT_TRUE(claim(store, sizes));
  ASSERT_EQ(kPopulateSizes.body, sizes.body);
  ASSERT_EQ(kPopulateSizes.heading, sizes.heading);
  ASSERT_TRUE(commitAll(store, kBatchA, std::size(kBatchA), sizes));
  ASSERT_TRUE(store.storePair('A', 'V', 0, -74));
  ASSERT_TRUE(commitMany(store));
  if (pending) fillSizeTable(store);
}

// Cuts the power at every step of a wipe. After the reboot either wipe is still pending (and the next wipe()
// finishes it), or the wipe had not begun, or it had completed; never half. Then the store goes on working.
void sweepWipe(const bool pending) {
  SimNorFlash dry(kFlashBytes);
  populate(dry, pending);
  if (::testing::Test::HasFatalFailure()) return;
  const uint64_t before = dry.steps();
  {
    FlashGlyphStore store(dry);
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
    ASSERT_TRUE(store.wipe());
  }
  const uint64_t wipeSteps = dry.steps() - before;
  ASSERT_GT(wipeSteps, 0u);
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < wipeSteps; ++cut) {
      const std::string where = std::string(pending ? "pending" : "not pending") + " wipe cut " + std::to_string(cut) +
                                (partial ? " partial" : "");
      SimNorFlash flash(kFlashBytes);
      populate(flash, pending);
      if (::testing::Test::HasFatalFailure()) return;
      {
        FlashGlyphStore store(flash);
        ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << where;
        flash.partialCut(partial);
        flash.cutAfterSteps(static_cast<int64_t>(cut));
        EXPECT_FALSE(store.wipe()) << where;
      }
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      FlashGlyphStore store(flash);
      ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << where;  // a wipe never touches the header
      checkGlyphs(store, kBatchA, std::size(kBatchA), kPopulateSizes, false, where);
      if (store.wipePending()) {
        ASSERT_TRUE(store.wipe()) << where;  // the next safe point finishes the job
        for (const GlyphSpec& g : kBatchA) {
          EXPECT_EQ(Lookup::NotStored, store.find(g.key(kPopulateSizes), nullptr)) << where;
        }
        EXPECT_FALSE(store.findPair('A', 'V', 0, nullptr)) << where;
      } else {
        // Not pending, so nothing will finish it: the wipe must not have begun, or must have completed.
        uint32_t intact = 0;
        uint32_t absent = 0;
        for (uint32_t i = 0; i < kMany; ++i) {
          const uint32_t key = makeKey(kManyFirst + i, 0, kPopulateSizes.body);
          if (storedIntact(store, key, 11, 16)) ++intact;
          if (store.find(key, nullptr) == Lookup::NotStored) ++absent;
        }
        EXPECT_TRUE(intact == kMany || absent == kMany) << where << ": intact " << intact << ", absent " << absent;
      }
      // The store goes on working, without programming any byte twice.
      Sizes sizes;
      ASSERT_TRUE(claim(store, sizes)) << where;
      ASSERT_TRUE(commitAll(store, kAfter, std::size(kAfter), sizes)) << where;
      EXPECT_EQ(0u, flash.violations()) << where;
      EXPECT_EQ(0u, flash.reprograms()) << where;
      if (::testing::Test::HasFailure()) return;
    }
  }
}

}  // namespace

TEST(GlyphStorePowerCut, EveryCutDuringASessionLeavesAConsistentStore) {
  SimNorFlash dry(kFlashBytes);
  const Outcome all = runSession(dry);
  ASSERT_TRUE(all.installed && all.batchA && all.noGlyph && all.batchB && all.pair);
  const uint64_t steps = dry.steps();
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < steps; ++cut) {
      SimNorFlash flash(kFlashBytes);
      flash.partialCut(partial);
      flash.cutAfterSteps(static_cast<int64_t>(cut));
      const Outcome out = runSession(flash);
      const std::string where = "cut " + std::to_string(cut) + (partial ? " partial" : "");
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      checkAfterReboot(flash, out, where);
      if (HasFailure()) return;  // one cut's report is enough to debug from
    }
  }
}

TEST(GlyphStorePowerCut, EveryCutDuringAWipeIsRecoverable) { sweepWipe(true); }

TEST(GlyphStorePowerCut, EveryCutDuringAWipeOfAStoreNotFullIsRecoverable) { sweepWipe(false); }

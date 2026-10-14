// A power cut at every step of every write (spec 4.5, 9.3). Whatever the store reported as stored is intact
// after the reboot, nothing is served torn, and writing carries on without programming any byte twice.
#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <string>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;
using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

struct GlyphSpec {
  uint32_t key;
  uint8_t width;
  uint8_t height;
};

const GlyphSpec kBatchA[] = {{makeKey('A', 0, 0), 9, 12}, {makeKey('B', 0, 0), 10, 12}, {makeKey(' ', 0, 0), 0, 0}};
const GlyphSpec kBatchB[] = {
    {makeKey('C', 0, 0), 11, 13}, {makeKey('d', 1, 0), 8, 12}, {makeKey(0x4E2D, 0, 1), 24, 26}};
const GlyphSpec kAfter[] = {{makeKey('E', 0, 0), 9, 13}, {makeKey('F', 0, 0), 9, 12}};
constexpr uint32_t kMissingKey = makeKey(0x2603, 0, 0);
constexpr uint32_t kPpem = 29 << 6;

// What each step of a session reported.
struct Outcome {
  bool installed = false;
  int sizeIndex = -1;
  bool batchA = false;
  bool missing = false;
  bool batchB = false;
  bool pair = false;
};

Layout sweepLayout() {
  Layout layout = smallLayout();
  layout.styleSize[0] = 256;  // a short style keeps the sweep fast
  return layout;
}

bool commitAll(FlashGlyphStore& store, const GlyphSpec* glyphs, const size_t count) {
  alignas(4) uint8_t buffer[2048];
  GlyphBatch batch(buffer, sizeof buffer);
  for (size_t i = 0; i < count; ++i) {
    if (!stage(batch, glyphs[i].key, glyphs[i].width, glyphs[i].height)) return false;
  }
  return store.commit(batch) == Commit::Stored;
}

// Every kind of write a session makes, from the install on.
Outcome runSession(SimNorFlash& flash) {
  Outcome out;
  FlashGlyphStore store(flash);
  out.installed = install(store, sweepLayout());
  if (!out.installed || store.open(kIdentityA) != OpenResult::Ready) return out;
  out.sizeIndex = store.sizeIndex(kPpem, 0);
  out.batchA = commitAll(store, kBatchA, std::size(kBatchA));
  out.missing = store.markMissing(kMissingKey) == Commit::Stored;
  out.batchB = commitAll(store, kBatchB, std::size(kBatchB));
  out.pair = store.storePair('A', 'V', 0, -74);
  return out;
}

void checkGlyphs(const FlashGlyphStore& store, const GlyphSpec* glyphs, const size_t count, const bool reported,
                 const std::string& where) {
  for (size_t i = 0; i < count; ++i) {
    const GlyphSpec& g = glyphs[i];
    if (reported) {
      EXPECT_TRUE(storedIntact(store, g.key, g.width, g.height)) << where << " key " << g.key;
    } else {
      EXPECT_TRUE(absentOrIntact(store, g.key, g.width, g.height)) << where << " key " << g.key;
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
  checkGlyphs(store, kBatchA, std::size(kBatchA), out.batchA, where);
  checkGlyphs(store, kBatchB, std::size(kBatchB), out.batchB, where);
  const Lookup missing = store.find(kMissingKey, nullptr);
  if (out.missing) {
    EXPECT_EQ(Lookup::Missing, missing) << where;
  } else {
    EXPECT_NE(Lookup::Found, missing) << where;
  }
  int16_t kern = 0;
  if (store.findPair('A', 'V', 0, &kern)) {
    EXPECT_EQ(-74, kern) << where;
  } else {
    EXPECT_FALSE(out.pair) << where;
  }
  if (out.sizeIndex >= 0) {
    EXPECT_EQ(out.sizeIndex, store.sizeIndex(kPpem, 0)) << where;
  }
  // The store carries on: a new batch lands, and everything is still there after another reboot.
  ASSERT_FALSE(store.wipePending()) << where;
  ASSERT_TRUE(commitAll(store, kAfter, std::size(kAfter))) << where;
  store.close();
  FlashGlyphStore again(flash);
  ASSERT_EQ(OpenResult::Ready, again.open(kIdentityA)) << where;
  checkGlyphs(again, kAfter, std::size(kAfter), true, where);
  EXPECT_EQ(0u, flash.violations()) << where;
}

// A store with data and wipe pending, as the full path leaves it.
void populate(SimNorFlash& flash) {
  FlashGlyphStore store(flash);
  ASSERT_TRUE(install(store, sweepLayout()));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_TRUE(commitAll(store, kBatchA, std::size(kBatchA)));
  ASSERT_TRUE(store.storePair('A', 'V', 0, -74));
  for (uint32_t i = 0; i < kSizeEntries; ++i) ASSERT_GE(store.sizeIndex((20 + i) << 6, 0), 0);
  ASSERT_EQ(-1, store.sizeIndex(99 << 6, 0));
  ASSERT_TRUE(store.wipePending());
}

}  // namespace

TEST(GlyphStorePowerCut, EveryCutDuringASessionLeavesAConsistentStore) {
  SimNorFlash dry(kFlashBytes);
  const Outcome all = runSession(dry);
  ASSERT_TRUE(all.installed && all.batchA && all.missing && all.batchB && all.pair && all.sizeIndex == 0);
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

TEST(GlyphStorePowerCut, EveryCutDuringAWipeIsRecoverable) {
  SimNorFlash dry(kFlashBytes);
  populate(dry);
  if (HasFatalFailure()) return;
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
      const std::string where = "wipe cut " + std::to_string(cut) + (partial ? " partial" : "");
      SimNorFlash flash(kFlashBytes);
      populate(flash);
      if (HasFatalFailure()) return;
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
      const OpenResult opened = store.open(kIdentityA);
      if (opened == OpenResult::NotInstalled) {
        // The cut hit the header rewrite: the family is installed again from scratch.
        ASSERT_TRUE(installAndOpen(store, sweepLayout())) << where;
      } else {
        ASSERT_EQ(OpenResult::Ready, opened) << where;
        checkGlyphs(store, kBatchA, std::size(kBatchA), false, where);
        if (store.wipePending()) {
          ASSERT_TRUE(store.wipe()) << where;
        }
      }
      // Either way the store is now empty and usable.
      for (const GlyphSpec& g : kBatchA) EXPECT_EQ(Lookup::Absent, store.find(g.key, nullptr)) << where;
      EXPECT_FALSE(store.findPair('A', 'V', 0, nullptr)) << where;
      EXPECT_EQ(0, store.sizeIndex(kPpem, 0)) << where;
      ASSERT_TRUE(commitAll(store, kAfter, std::size(kAfter))) << where;
      EXPECT_EQ(0u, flash.violations()) << where;
      if (HasFailure()) return;
    }
  }
}

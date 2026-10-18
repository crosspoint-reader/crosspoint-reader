#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "EpdFontData.h"
#include "FlashGlyphStore.h"
#include "GlyphStoreFormat.h"
#include "SimNorFlash.h"

namespace gst_test {

using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

constexpr uint8_t kIdentityA[glyphstore::kIdentityBytes] = {'f', 'a', 'm', 'i', 'l', 'y', '-', 'A'};
constexpr uint8_t kIdentityB[glyphstore::kIdentityBytes] = {'f', 'a', 'm', 'i', 'l', 'y', '-', 'B'};
constexpr uint32_t kFlashBytes = 262144;
/// installAndOpen() claims this many sizes, so the size indices 0-3 the tests' keys use are valid.
constexpr uint8_t kTestSizes = 4;
constexpr uint32_t testPpem(const uint8_t size) { return (40u + size) << 6; }

/// A small store: one 4 KB style at 64 KB, then the index, the pair index and the log, each on a sector.
inline glyphstore::Layout smallLayout(const uint32_t indexSlots = 1024, const uint32_t pairSlots = 512,
                                      const uint32_t logBytes = 65536) {
  glyphstore::Layout layout{};
  glyphstore::Geometry& g = layout.geometry;
  g.mode = glyphstore::kModeFlash;
  g.styleCount = 1;
  layout.styleOffset[0] = 65536;
  layout.styleSize[0] = 4096;
  g.indexOffset = 131072;
  g.indexSlots = indexSlots;
  g.pairOffset = g.indexOffset + glyphstore::alignUp(indexSlots * glyphstore::kSlotBytes, glyphstore::kSectorBytes);
  g.pairSlots = pairSlots;
  g.logOffset = g.pairOffset + glyphstore::alignUp(pairSlots * glyphstore::kSlotBytes, glyphstore::kSectorBytes);
  g.logSize = logBytes;
  return layout;
}

inline uint8_t styleByte(const uint8_t style, const uint32_t i) { return static_cast<uint8_t>(i * 7 + style * 13 + 1); }

/// Installs `layout` under `identity`, every style filled with styleByte().
inline bool install(FlashGlyphStore& store, const glyphstore::Layout& layout, const uint8_t* identity = kIdentityA,
                    const glyphstore::StyleInfo* sdInfo = nullptr) {
  if (!store.beginInstall(layout, identity, sdInfo)) return false;
  for (uint8_t s = 0; s < layout.geometry.styleCount; ++s) {
    std::vector<uint8_t> data(layout.styleSize[s]);
    for (uint32_t i = 0; i < data.size(); ++i) data[i] = styleByte(s, i);
    if (!store.writeStyleData(s, 0, data.data(), static_cast<uint32_t>(data.size()))) return false;
  }
  return store.finishInstall();
}

/// Claims size indices 0 to kTestSizes - 1 on a store with an empty size table.
inline bool claimTestSizes(FlashGlyphStore& store) {
  for (uint8_t size = 0; size < kTestSizes; ++size) {
    uint8_t index = 0xFF;
    if (!store.faceIndex(0, testPpem(size), &index) || index != size) return false;
  }
  return true;
}

/// Installs, opens, and claims the test sizes.
inline bool installAndOpen(FlashGlyphStore& store, const glyphstore::Layout& layout,
                           const uint8_t* identity = kIdentityA) {
  return install(store, layout, identity) && store.open(identity) == OpenResult::Ready && claimTestSizes(store);
}

/// Claims sizes until the table is full; the store is then full, with wipe pending.
inline void fillSizeTable(FlashGlyphStore& store) {
  uint8_t index = 0;
  for (uint32_t i = 0; i < glyphstore::kFaceEntries; ++i) store.faceIndex(0, (100 + i) << 6, &index);
  ASSERT_FALSE(store.faceIndex(0, 99 << 6, &index));
  ASSERT_TRUE(store.wipePending());
}

inline EpdGlyph metrics(const uint8_t width, const uint8_t height) {
  EpdGlyph glyph{};
  glyph.width = width;
  glyph.height = height;
  glyph.advanceX = static_cast<uint16_t>((width + 1) << 4);
  glyph.left = 1;
  glyph.top = height;
  glyph.dataLength = glyphDataBytes(width, height, true);
  return glyph;
}

/// Bitmap byte i of the glyph stored under `key`: a pattern any test can recompute.
inline uint8_t pattern(const uint32_t key, const uint32_t i) {
  return static_cast<uint8_t>((key * 31u + i * 7u) ^ 0x5Au);
}

/// Bytes a record with a width x height bitmap takes in the log.
inline uint32_t recordSize(const uint8_t width, const uint8_t height) {
  return glyphstore::align4(glyphstore::kRecordHeaderBytes + glyphDataBytes(width, height, true));
}

/// Programs a record straight into the log at `offset`, bypassing the store (the recovery tests). Only the
/// first `bitmapBytes` of its bitmap are written, as a cut write would leave it.
inline void writeRawRecord(SimNorFlash& flash, const glyphstore::Layout& layout, const uint32_t offset,
                           const uint32_t key, const uint8_t width, const uint8_t height,
                           const uint32_t bitmapBytes = UINT32_MAX) {
  EpdGlyph glyph = metrics(width, height);
  glyph.dataOffset = offset + glyphstore::kRecordHeaderBytes;
  std::vector<uint8_t> record(glyphstore::kRecordHeaderBytes + glyph.dataLength);
  std::memcpy(record.data(), &key, sizeof key);
  std::memcpy(record.data() + 4, &glyph, sizeof glyph);
  for (uint32_t i = 0; i < glyph.dataLength; ++i) record[glyphstore::kRecordHeaderBytes + i] = pattern(key, i);
  const uint32_t written = bitmapBytes < glyph.dataLength ? bitmapBytes : glyph.dataLength;
  ASSERT_TRUE(
      flash.program(layout.geometry.logOffset + offset, record.data(), glyphstore::kRecordHeaderBytes + written));
}

/// Programs only a record header, with `glyph` as given (the recovery tests' malformed records).
inline void writeRawHeader(SimNorFlash& flash, const glyphstore::Layout& layout, const uint32_t offset,
                           const uint32_t key, const EpdGlyph& glyph) {
  uint8_t header[glyphstore::kRecordHeaderBytes];
  std::memcpy(header, &key, sizeof key);
  std::memcpy(header + 4, &glyph, sizeof glyph);
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + offset, header, sizeof header));
}

/// Stages `key` as a width x height glyph whose bitmap is pattern(key, i).
inline bool stage(GlyphBatch& batch, const uint32_t key, const uint8_t width, const uint8_t height) {
  const EpdGlyph glyph = metrics(width, height);
  uint8_t* bitmap = batch.add(key, glyph);
  if (!bitmap) return false;
  for (uint32_t i = 0; i < glyph.dataLength; ++i) bitmap[i] = pattern(key, i);
  return true;
}

/// True when `key` is stored with exactly the metrics and bitmap stage() gave it.
inline bool storedIntact(const FlashGlyphStore& store, const uint32_t key, const uint8_t width, const uint8_t height) {
  const EpdGlyph* glyph = nullptr;
  if (store.find(key, &glyph) != Lookup::Found || glyph == nullptr) return false;
  const EpdGlyph want = metrics(width, height);
  if (glyph->width != want.width || glyph->height != want.height || glyph->advanceX != want.advanceX ||
      glyph->left != want.left || glyph->top != want.top || glyph->dataLength != want.dataLength) {
    return false;
  }
  const uint8_t* bitmap = store.logBase() + glyph->dataOffset;
  for (uint32_t i = 0; i < glyph->dataLength; ++i) {
    if (bitmap[i] != pattern(key, i)) return false;
  }
  return true;
}

/// Not stored, or stored intact: a lookup may miss a glyph whose write was cut, but never serve wrong bytes.
inline bool absentOrIntact(const FlashGlyphStore& store, const uint32_t key, const uint8_t width,
                           const uint8_t height) {
  return store.find(key, nullptr) == Lookup::NotStored || storedIntact(store, key, width, height);
}

/// Every store test runs on its own simulated chip, and no test may ever program a byte twice.
class StoreTest : public ::testing::Test {
 protected:
  void TearDown() override {
    EXPECT_EQ(0u, flash.violations());
    EXPECT_EQ(0u, flash.reprograms());
  }

  SimNorFlash flash{kFlashBytes};
  FlashGlyphStore store{flash};
};

}  // namespace gst_test

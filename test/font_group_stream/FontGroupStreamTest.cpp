// The contract that makes EpdFontGroup::ringBytes safe to trust.
//
// A group is no longer inflated into a buffer of its uncompressed size; it is streamed through
// a ring of ringBytes and each glyph is compacted out of the passing bytes. That is only sound
// if two things hold, and both are checked here against real shipped font data rather than a
// synthetic stream:
//
//   1. ringBytes is SUFFICIENT — every group decodes byte-for-byte through a ring that size.
//   2. ringBytes is HONEST — it is genuinely smaller than the group for the big groups, which
//      is the entire reason the field exists. A converter that quietly emitted
//      ringBytes == uncompressedSize would pass (1) while delivering nothing.
//
// And the failure mode is checked too: a ring that is too small must be REFUSED, not decoded
// into garbage. uzlib returns TINF_DICT_ERROR for a back-reference that outruns the ring, so a
// mis-generated font cannot silently render wrong pixels.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "BuildArena.h"
#include "EpdFontData.h"
#include "FontDecompressor.h"
#include "GlyphProvider.h"

// A real generated font: 19 groups, uncompressed sizes up to ~25 KB, rings measured from the
// finished DEFLATE streams by fontconvert.py.
#include "notosans_14_regular.h"

// The font with the largest glyph in the built-in set (54x38, 513 packed bytes), which used to
// overflow HOT_GLYPH_BUF_SIZE by one byte.
#include "bookerly_18_bolditalic.h"

namespace {

struct Decoded {
  bool ok = false;
  std::vector<uint8_t> bytes;  // every glyph of the group, packed, in glyph order
  uint32_t consumed = 0;       // bytes of the group the stream decoded to get them
};

// Pull every glyph of `group` through the real FontDecompressor::GroupStream with a ring of
// exactly `ringBytes`, in ascending order as prewarmCache and getBitmap do. The stream reaches
// each glyph by skipping 64 bytes at a time, so the ring carries the back-reference history
// across many separate reads.
Decoded DecodeThroughRing(const EpdFontData& font, const EpdFontGroup& group, uint32_t ringBytes) {
  std::vector<uint8_t> ring(ringBytes);
  FontDecompressor::GroupStream stream;
  if (!stream.begin(&font, group, ring.data(), ringBytes)) return {};

  Decoded out;
  uint32_t alignedOffset = 0;
  for (uint32_t i = group.firstGlyphIndex; i < group.firstGlyphIndex + group.glyphCount; i++) {
    const EpdGlyphRef glyph = epdResolveGlyph(&font, i);
    const size_t at = out.bytes.size();
    out.bytes.resize(at + glyphDataBytes(glyph.width, glyph.height, font.is2Bit));
    if (!stream.extractGlyph(alignedOffset, glyph, out.bytes.data() + at)) return {};
    // Byte-aligned rows, the layout fontconvert.py emits (see FontDecompressor::getAlignedOffset).
    alignedOffset += (glyph.width + 3u) / 4u * glyph.height;
  }
  out.ok = true;
  out.consumed = stream.consumed();
  return out;
}

// The shipped font as the reader sees it: contiguous groups, 2-bit, compressed.
const EpdFontData& Font() { return notosans_14_regular; }

const EpdFontGroup* Groups() { return notosans_14_regularGroups; }
constexpr uint16_t kGroupCount = sizeof(notosans_14_regularGroups) / sizeof(notosans_14_regularGroups[0]);

}  // namespace

// (1) Every group decodes correctly through exactly the ring the converter promised.
TEST(FontGroupStream, RingBytesIsSufficient) {
  for (uint16_t i = 0; i < kGroupCount; i++) {
    const EpdFontGroup& g = Groups()[i];
    ASSERT_GT(g.ringBytes, 0u) << "group " << i << " has no measured ring";
    ASSERT_LE(g.ringBytes, g.uncompressedSize) << "group " << i << " claims a ring larger than itself";

    const Decoded ring = DecodeThroughRing(Font(), g, g.ringBytes);
    ASSERT_TRUE(ring.ok) << "group " << i << " failed to decode through its own " << g.ringBytes << "-byte ring";
    // Its last glyph ends the group, so the ring carried the whole stream, not a prefix.
    EXPECT_EQ(ring.consumed, g.uncompressedSize) << "group " << i;

    // Ground truth: the same stream decoded with a full 32 KB window.
    const Decoded full = DecodeThroughRing(Font(), g, 32768);
    ASSERT_TRUE(full.ok) << "group " << i << " failed to decode at all";
    EXPECT_EQ(ring.bytes, full.bytes) << "group " << i << " decoded differently through its ring";
  }
}

// (2) The field actually buys something. Without this the whole change is a no-op that still
// passes every correctness check.
TEST(FontGroupStream, RingIsSmallerThanTheGroup) {
  uint32_t peakRing = 0;
  uint32_t peakGroup = 0;
  for (uint16_t i = 0; i < kGroupCount; i++) {
    peakRing = std::max(peakRing, static_cast<uint32_t>(Groups()[i].ringBytes));
    peakGroup = std::max(peakGroup, Groups()[i].uncompressedSize);
  }
  // The transient the reader pays is the peak ring, not the peak group.
  EXPECT_LT(peakRing, peakGroup) << "rings are no cheaper than inflating whole groups";
  // GROUP_RING_MAX_BYTES in fontconvert.py. The converter enforces it; this is the on-device
  // half of that contract, and it is what bounds the transient allocation.
  EXPECT_LE(peakRing, 4096u) << "a group exceeds the ring ceiling the decoder budgets for";
}

// Every glyph must fit the fallback cache slot. A glyph that does not is not a crash and not a
// test failure anywhere else — getBitmap simply refuses it and the character renders blank on
// any page whose prewarm missed it, which is how one 513-byte glyph went unnoticed against a
// 512-byte buffer. fontconvert.py enforces the same bound when generating; this is the shipped-
// data side of that contract, so a hand-edited or stale header cannot reintroduce it.
TEST(FontGroupStream, EveryGlyphFitsTheFallbackSlot) {
  struct FontUnderTest {
    const char* name;
    const EpdGlyphPacked* glyphs;
    size_t count;
  };
  const FontUnderTest fonts[] = {
      {"bookerly_18_bolditalic", bookerly_18_bolditalicGlyphs,
       sizeof(bookerly_18_bolditalicGlyphs) / sizeof(bookerly_18_bolditalicGlyphs[0])},
      {"notosans_14_regular", notosans_14_regularGlyphs,
       sizeof(notosans_14_regularGlyphs) / sizeof(notosans_14_regularGlyphs[0])},
  };

  for (const auto& f : fonts) {
    uint16_t largest = 0;
    for (size_t i = 0; i < f.count; i++) {
      // Derived, not stored -- see EpdGlyphPacked. Both faces here are 2-bit.
      const uint16_t dataLength = glyphDataBytes(f.glyphs[i].width, f.glyphs[i].height, /*is2Bit=*/true);
      largest = std::max(largest, dataLength);
      ASSERT_LE(dataLength, FontDecompressor::HOT_GLYPH_BUF_SIZE)
          << f.name << " glyph " << i << " (" << +f.glyphs[i].width << "x" << +f.glyphs[i].height
          << ") cannot be served by the fallback cache and will render blank";
    }
    EXPECT_GT(largest, 0u) << f.name << " has no glyph data";
  }
}

// A ring too small must fail loudly. This is what stops a bad ringBytes from turning into
// wrong pixels instead of a visible error.
TEST(FontGroupStream, TooSmallRingIsRefusedNotCorrupted) {
  // The largest group is the one whose references certainly outrun a tiny ring.
  uint16_t widest = 0;
  for (uint16_t i = 1; i < kGroupCount; i++) {
    if (Groups()[i].ringBytes > Groups()[widest].ringBytes) widest = i;
  }
  const EpdFontGroup& g = Groups()[widest];
  ASSERT_GT(g.ringBytes, 512u) << "fixture has no group with a reach worth truncating";

  const Decoded good = DecodeThroughRing(Font(), g, g.ringBytes);
  ASSERT_TRUE(good.ok);

  const Decoded starved = DecodeThroughRing(Font(), g, 256);
  // Either uzlib refuses it (the expected TINF_DICT_ERROR path) or -- if this particular
  // stream happens to survive -- the bytes must still be right. Silent corruption is the
  // only outcome that is not allowed.
  if (starved.ok) {
    EXPECT_EQ(starved.bytes, good.bytes) << "a starved ring produced different bytes without erroring";
  }
}

// --- The prewarm's rings in a lent region ------------------------------------------------------

namespace {

constexpr const char* kPageText = "The quick brown fox jumps over the lazy dog, 1234567890 times: \u00e9\u00e8\u00fc!";

// The glyph index of `cp`, as EpdFont's interval lookup finds it; -1 when the font has none.
int32_t glyphIndexOf(const EpdFontData& font, const uint32_t cp) {
  for (uint32_t i = 0; i < font.intervalCount; i++) {
    const EpdUnicodeInterval& in = font.intervals[i];
    if (cp >= in.first && cp <= in.last) return static_cast<int32_t>(in.offset + (cp - in.first));
  }
  return -1;
}

// Every inked glyph of the page text as getBitmap() hands it out, concatenated.
std::vector<uint8_t> pageBitmaps(FontDecompressor& fdc, const EpdFontData& font) {
  std::vector<uint8_t> out;
  for (const char* p = kPageText; *p != '\0'; ++p) {
    const auto byte = static_cast<unsigned char>(*p);
    if (byte >= 0x80) continue;  // ASCII only here; the prewarm warms the accented ones too
    const int32_t index = glyphIndexOf(font, byte);
    if (index < 0) continue;
    const EpdGlyphRef glyph = epdResolveGlyph(&font, static_cast<uint32_t>(index));
    const uint16_t bytes = glyphDataBytes(glyph.width, glyph.height, font.is2Bit);
    if (bytes == 0) continue;
    const uint8_t* bitmap = fdc.getBitmap(&font, glyph, static_cast<uint32_t>(index));
    if (!bitmap) return {};
    out.insert(out.end(), bitmap, bitmap + bytes);
  }
  return out;
}

}  // namespace

// A page render lends the secondary framebuffer to its prewarm (glyph_miss::Scope): each group's ring comes from it and
// goes back before the next group, so the region is as it was when the lend ends, and the page slots, which the render
// and the AA pass read after the lend, never come from it.
TEST(FontPrewarmRing, TheRingsComeFromTheLentRegionAndThePageSlotsFromTheHeap) {
  const EpdFontData& font = Font();
  FontDecompressor onHeap;
  ASSERT_TRUE(onHeap.init());
  ASSERT_EQ(0, onHeap.prewarmCache(&font, kPageText));
  const FontDecompressor::Stats heapStats = onHeap.getStats();
  ASSERT_GT(heapStats.uniqueGroupsAccessed, 0u);
  EXPECT_EQ(0u, heapStats.arenaTemps);  // no region lent: every ring on the heap, as before

  std::vector<uint8_t> secondary(48000);
  BuildArena region(secondary.data(), secondary.size());
  FontDecompressor lent;
  ASSERT_TRUE(lent.init());
  {
    const glyph_miss::Scope misses(&region);
    ASSERT_EQ(0, lent.prewarmCache(&font, kPageText));
  }
  const FontDecompressor::Stats& lentStats = lent.getStats();
  EXPECT_EQ(heapStats.uniqueGroupsAccessed, lentStats.arenaTemps);  // every ring from the region
  EXPECT_EQ(0u, region.used());                                     // and given back
  EXPECT_EQ(heapStats.peakTempBytes, lentStats.peakTempBytes);
  EXPECT_GE(region.highWater(), lentStats.peakTempBytes);
  EXPECT_FALSE(lent.hasArenaBackedSlots());
  EXPECT_EQ(heapStats.pageBufferBytes, lentStats.pageBufferBytes);  // the slots: on the heap, the same
  std::printf(
      "[  heap    ] prewarm of %u groups: %lu B page slots on the heap; largest ring %lu B, now in the lent "
      "region\n",
      static_cast<unsigned>(lentStats.uniqueGroupsAccessed),
      static_cast<unsigned long>(lentStats.pageBufferBytes + lentStats.pageGlyphsBytes),
      static_cast<unsigned long>(lentStats.peakTempBytes));

  // The display has the buffer again and draws over it: the page's glyphs still come from the slots, as on the heap.
  std::memset(secondary.data(), 0x5A, secondary.size());
  const std::vector<uint8_t> expected = pageBitmaps(onHeap, font);
  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(expected, pageBitmaps(lent, font));
  EXPECT_EQ(0u, lent.getStats().fallbackCacheMisses);  // all from the page slots, no ring needed
}

// A region with no room for a ring leaves it to the heap, as before the lend.
TEST(FontPrewarmRing, ARegionWithoutRoomLeavesTheRingToTheHeap) {
  std::vector<uint8_t> small(64);
  BuildArena region(small.data(), small.size());
  FontDecompressor fdc;
  ASSERT_TRUE(fdc.init());
  {
    const glyph_miss::Scope misses(&region);
    EXPECT_EQ(0, fdc.prewarmCache(&Font(), kPageText));
  }
  EXPECT_EQ(0u, fdc.getStats().arenaTemps);
  EXPECT_EQ(0u, region.used());
}

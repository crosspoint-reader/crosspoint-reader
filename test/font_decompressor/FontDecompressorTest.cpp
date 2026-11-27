#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <FontDecompressor.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdlib>
#include <vector>

// Vendored uzlib lacks checksum sources; font groups are raw DEFLATE and never call them.
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t) { std::abort(); }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t) { std::abort(); }

namespace {

// Byte-aligned 2-bit rows, as fontconvert stores them before DEFLATE. Each glyph
// is 5x1, so a row spans two bytes and getBitmap must repack it.
constexpr uint8_t kAligned[] = {0x11, 0x40, 0x22, 0x80, 0x33, 0xC0};

std::vector<uint8_t> rawDeflate(const uint8_t* data, size_t size) {
  z_stream zs{};
  EXPECT_EQ(deflateInit2(&zs, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY), Z_OK);
  std::vector<uint8_t> out(deflateBound(&zs, size));
  zs.next_in = const_cast<uint8_t*>(data);
  zs.avail_in = size;
  zs.next_out = out.data();
  zs.avail_out = out.size();
  EXPECT_EQ(deflate(&zs, Z_FINISH), Z_STREAM_END);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

}  // namespace

// Compact records decode into caller scratch, so getBitmap cannot recover the glyph
// index from the pointer. The index must come from the lookup that found the glyph,
// including when the glyph is served by a family's fallback font.
TEST(FontDecompressor, CompactCompressedGlyphsUseTheLookupIndex) {
  const auto compressed = rawDeflate(kAligned, sizeof(kAligned));
  const EpdBuiltinGlyph glyphs[] = {{5, 1, 80, 0, 1, 0}, {5, 1, 80, 0, 1, 2}, {5, 1, 80, 0, 1, 4}};
  const EpdUnicodeInterval intervals[] = {{'A', 'C', 0}};
  const EpdFontGroup groups[] = {{0, static_cast<uint32_t>(compressed.size()), sizeof(kAligned), 3, 0}};
  EpdFontData builtinData{};
  builtinData.bitmap = compressed.data();
  builtinData.builtinGlyphs = glyphs;
  builtinData.intervals = intervals;
  builtinData.intervalCount = 1;
  builtinData.is2Bit = true;
  builtinData.groups = groups;
  builtinData.groupCount = 1;
  const EpdFont builtinFont(&builtinData);
  const EpdFontFamily builtin(12, &builtinFont);

  // The primary face only covers 'A', so 'B' and 'C' come from the built-in fallback.
  const EpdGlyph customGlyphs[] = {{5, 1, 80, 0, 1, 2, 0}};
  const EpdUnicodeInterval customIntervals[] = {{'A', 'A', 0}};
  const uint8_t customBitmap[] = {0xFF, 0xC0};
  EpdFontData customData{};
  customData.bitmap = customBitmap;
  customData.glyph = customGlyphs;
  customData.intervals = customIntervals;
  customData.intervalCount = 1;
  customData.is2Bit = true;
  const EpdFont customFont(&customData);
  EpdFontFamily family(12, &customFont);
  family.setGlyphFallback(&builtin);

  FontDecompressor fd;
  ASSERT_TRUE(fd.init());
  const uint8_t expected[][2] = {{0x22, 0x80}, {0x33, 0xC0}};
  for (int pass = 0; pass < 2; pass++) {
    // Pass 0 decompresses through the hot group; pass 1 reads the page buffer.
    if (pass == 1) ASSERT_EQ(fd.prewarmCache(&builtinData, "BC"), 0);
    for (uint32_t i = 0; i < 2; i++) {
      EpdGlyph scratch;
      GlyphBitmapSource source;
      const EpdGlyph* glyph = family.getGlyph('B' + i, scratch, EpdFontFamily::REGULAR, &source);
      ASSERT_EQ(glyph, &scratch);
      ASSERT_EQ(source.font, &builtinData);
      EXPECT_EQ(source.index, i + 1);
      EXPECT_EQ(glyph->dataLength, 2);
      const uint8_t* bitmap = fd.getBitmap(source.font, glyph, source.index);
      ASSERT_NE(bitmap, nullptr);
      EXPECT_EQ(bitmap[0], expected[i][0]) << "pass " << pass << " glyph " << i;
      EXPECT_EQ(bitmap[1], expected[i][1]) << "pass " << pass << " glyph " << i;
    }
  }
}

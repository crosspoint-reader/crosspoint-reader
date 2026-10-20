// The compressed CJK UI faces decode to exactly the pixels they had uncompressed.
//
// compress_ui_face.py turned the six 1-bit CJK UI faces' flat bitmaps into DEFLATE groups. Nothing
// about a glyph may change by that: its metrics stay in the untouched glyph table, so what is left
// to prove is the bitmap, and it is proved here against the bytes master shipped, through both
// paths the device decodes by:
//   * FontDecompressor::inflateFace(), the whole face in glyph order -- what the language pack slot
//     receives for the active UI language (CjkUiFonts);
//   * FontDecompressor::getBitmap(), one glyph at a time with nothing cached -- what the language
//     picker and a CJK book title use for the other two languages.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "FontDecompressor.h"
#include "builtinFonts/cjk_ui_ja_10.h"
#include "builtinFonts/cjk_ui_ja_12.h"
#include "builtinFonts/cjk_ui_ko_10.h"
#include "builtinFonts/cjk_ui_ko_12.h"
#include "builtinFonts/cjk_ui_zh_10.h"
#include "builtinFonts/cjk_ui_zh_12.h"

namespace {

// zlib's CRC-32, which is what compress_ui_face.py and the master figures below were taken with.
uint32_t crc32(const uint8_t* data, const size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

struct Face {
  const char* name;
  const EpdFontData* data;
  uint32_t glyphCount;
  // The flat bitmap master shipped (origin/master 617216ad9, before compress_ui_face.py), measured
  // from those headers' <name>Bitmaps arrays: the ground truth, independent of the transform.
  uint32_t masterBytes;
  uint32_t masterCrc32;
  // What the transform wrote into the header, checked against the master figures.
  uint32_t packedBytes;
  uint32_t packedCrc32;
};

const Face FACES[] = {
    {"cjk_ui_zh_10", &cjk_ui_zh_10, 599, 29674, 0xA1713AABu, cjk_ui_zh_10PackedBytes, cjk_ui_zh_10PackedCrc32},
    {"cjk_ui_zh_12", &cjk_ui_zh_12, 599, 41096, 0x932D78E8u, cjk_ui_zh_12PackedBytes, cjk_ui_zh_12PackedCrc32},
    {"cjk_ui_ja_10", &cjk_ui_ja_10, 500, 23253, 0xF13A6D84u, cjk_ui_ja_10PackedBytes, cjk_ui_ja_10PackedCrc32},
    {"cjk_ui_ja_12", &cjk_ui_ja_12, 500, 32643, 0xEDEEC430u, cjk_ui_ja_12PackedBytes, cjk_ui_ja_12PackedCrc32},
    {"cjk_ui_ko_10", &cjk_ui_ko_10, 442, 17696, 0x90EF8803u, cjk_ui_ko_10PackedBytes, cjk_ui_ko_10PackedCrc32},
    {"cjk_ui_ko_12", &cjk_ui_ko_12, 442, 25390, 0xEADD97D7u, cjk_ui_ko_12PackedBytes, cjk_ui_ko_12PackedCrc32},
};

bool appendTo(void* ctx, const uint8_t* bytes, const uint32_t len) {
  auto* out = static_cast<std::vector<uint8_t>*>(ctx);
  out->insert(out->end(), bytes, bytes + len);
  return true;
}

std::vector<uint8_t> inflate(const EpdFontData* face) {
  std::vector<uint8_t> ring(FontDecompressor::maxRingBytes(face));
  std::vector<uint8_t> glyph(FontDecompressor::HOT_GLYPH_BUF_SIZE);
  std::vector<uint8_t> out;
  if (!FontDecompressor::inflateFace(face, ring.data(), static_cast<uint32_t>(ring.size()), glyph.data(), &appendTo,
                                     &out)) {
    out.clear();
  }
  return out;
}

class EveryCjkUiFace : public ::testing::TestWithParam<Face> {};

TEST_P(EveryCjkUiFace, IsCompressedOneBitAndNeedsAtMostATwoKilobyteRing) {
  const Face& f = GetParam();
  ASSERT_NE(f.data->groups, nullptr);
  EXPECT_FALSE(f.data->is2Bit);
  EXPECT_EQ(f.data->bitmapOffsets, nullptr) << "a compressed face finds its glyphs by the group walk";
  // The groups tile the glyph array, so the last one ends at the glyph count.
  const EpdFontGroup& last = f.data->groups[f.data->groupCount - 1];
  EXPECT_EQ(last.firstGlyphIndex + last.glyphCount, f.glyphCount);
  // The transient one decode costs: a group is at most 2 KB, and its ring can never exceed it.
  EXPECT_LE(FontDecompressor::maxRingBytes(f.data), 2048u);
}

TEST_P(EveryCjkUiFace, TheHeaderStatesTheBitmapMasterShipped) {
  const Face& f = GetParam();
  EXPECT_EQ(f.packedBytes, f.masterBytes);
  EXPECT_EQ(f.packedCrc32, f.masterCrc32);
}

TEST_P(EveryCjkUiFace, InflatesToTheBitmapMasterShipped) {
  const Face& f = GetParam();
  const std::vector<uint8_t> flat = inflate(f.data);
  ASSERT_EQ(flat.size(), f.masterBytes);
  EXPECT_EQ(crc32(flat.data(), flat.size()), f.masterCrc32);
}

TEST_P(EveryCjkUiFace, EveryGlyphDecodesAloneToItsBytesInTheFlatBitmap) {
  const Face& f = GetParam();
  const std::vector<uint8_t> flat = inflate(f.data);
  ASSERT_EQ(flat.size(), f.masterBytes);

  FontDecompressor d;
  ASSERT_TRUE(d.init());
  uint32_t offset = 0;
  for (uint32_t i = 0; i < f.glyphCount; ++i) {
    const EpdGlyphRef glyph = epdResolveGlyph(f.data, i);
    const uint16_t len = glyphDataBytes(glyph.width, glyph.height, false);
    if (len == 0) continue;
    // Cold every time: other glyphs evict it from the four fallback slots long before it comes round
    // again, and the page slots are never prewarmed for a UI face.
    const uint8_t* bitmap = d.getBitmap(f.data, glyph, i);
    ASSERT_NE(bitmap, nullptr) << f.name << " glyph " << i;
    ASSERT_EQ(std::memcmp(bitmap, flat.data() + offset, len), 0) << f.name << " glyph " << i;
    offset += len;
  }
  EXPECT_EQ(offset, f.masterBytes);
  EXPECT_EQ(d.getStats().fallbackOomGlyphs, 0u);
}

TEST_P(EveryCjkUiFace, AGlyphLooksUpByCodepointAsBefore) {
  // The interval table is untouched: the box every face carries is still where it was.
  const Face& f = GetParam();
  const EpdGlyphRef box = EpdFont(f.data).findGlyph(0x25A1);
  ASSERT_TRUE(box);
  EXPECT_EQ(box.index, 0u);
  EXPECT_GT(box.width, 0);
}

INSTANTIATE_TEST_SUITE_P(Faces, EveryCjkUiFace, ::testing::ValuesIn(FACES),
                         [](const ::testing::TestParamInfo<Face>& info) { return std::string(info.param.name); });

// --- inflateFace refusals: each would otherwise write a wrong face into the language pack slot ---

TEST(InflateFace, RefusesARingSmallerThanAGroupNeeds) {
  const uint32_t need = FontDecompressor::maxRingBytes(&cjk_ui_zh_12);
  std::vector<uint8_t> ring(need);
  std::vector<uint8_t> glyph(FontDecompressor::HOT_GLYPH_BUF_SIZE);
  std::vector<uint8_t> out;
  EXPECT_FALSE(FontDecompressor::inflateFace(&cjk_ui_zh_12, ring.data(), need - 1, glyph.data(), &appendTo, &out));
}

TEST(InflateFace, RefusesAnUncompressedFace) {
  EpdFontData flat = cjk_ui_zh_10;
  flat.groups = nullptr;
  flat.groupCount = 0;
  std::vector<uint8_t> ring(4096), glyph(FontDecompressor::HOT_GLYPH_BUF_SIZE);
  std::vector<uint8_t> out;
  EXPECT_FALSE(FontDecompressor::inflateFace(&flat, ring.data(), 4096, glyph.data(), &appendTo, &out));
}

TEST(InflateFace, StopsWhenTheSinkRefuses) {
  std::vector<uint8_t> ring(FontDecompressor::maxRingBytes(&cjk_ui_ko_10));
  std::vector<uint8_t> glyph(FontDecompressor::HOT_GLYPH_BUF_SIZE);
  uint32_t calls = 0;
  const auto refuseThird = [](void* ctx, const uint8_t*, uint32_t) { return ++*static_cast<uint32_t*>(ctx) < 3; };
  EXPECT_FALSE(FontDecompressor::inflateFace(&cjk_ui_ko_10, ring.data(), static_cast<uint32_t>(ring.size()),
                                             glyph.data(), refuseThird, &calls));
  EXPECT_EQ(calls, 3u);
}

}  // namespace

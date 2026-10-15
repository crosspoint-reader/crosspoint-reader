// parseFace, glyphIndex and the metrics: what the engine reads from a font file, and the converter's sizes.
#include <gtest/gtest.h>

#include <cstdint>

#include "TtfBuilder.h"
#include "TtfFace.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

// .notdef, 'A' and 'B': three glyphs of different widths.
Font threeGlyphs() {
  Font font;
  font.glyphs.resize(3);
  font.glyphs[0].contours = {rect(50, 0, 450, 700)};
  font.glyphs[1].contours = {rect(20, 0, 620, 700)};
  font.glyphs[1].advance = 640;
  font.glyphs[2].contours = {rect(60, 0, 560, 700), rect(160, 100, 460, 600)};
  font.glyphs[2].advance = 600;
  font.cmap = {{'A', 1}, {'B', 2}};
  return font;
}

FaceStatus parse(const Bytes& data, Face* face) {
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  return parseFace(bytes, face);
}

uint16_t lookup(const Bytes& data, const uint32_t codepoint) {
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  if (parseFace(bytes, &face) != FaceStatus::Ok) return 0xFFFF;
  return glyphIndex(bytes, face, codepoint);
}

}  // namespace

TEST(TtfFace, ParsesTheTablesAGlyphNeeds) {
  Font font = threeGlyphs();
  font.longLoca = true;
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  EXPECT_EQ(1000, face.unitsPerEm);
  EXPECT_EQ(3, face.numGlyphs);
  EXPECT_EQ(3, face.numHMetrics);
  EXPECT_TRUE(face.longLoca);
  EXPECT_EQ(8, face.maxPoints);
  EXPECT_EQ(2, face.maxContours);
  EXPECT_EQ(20, face.xMin);
  EXPECT_EQ(0, face.yMin);
  EXPECT_EQ(620, face.xMax);
  EXPECT_EQ(700, face.yMax);
  EXPECT_EQ(800, face.ascender);
  EXPECT_EQ(-200, face.descender);
  EXPECT_EQ(1000, face.height);
  EXPECT_EQ(4, face.cmapFormat);
}

TEST(TtfFace, CffFontsAndCollectionsAreUnsupported) {
  Font font = threeGlyphs();
  font.sfntVersion = tagOf("OTTO");
  Face face;
  EXPECT_EQ(FaceStatus::Unsupported, parse(buildFont(font), &face));
  font.sfntVersion = tagOf("ttcf");
  EXPECT_EQ(FaceStatus::Unsupported, parse(buildFont(font), &face));
  font.sfntVersion = tagOf("true");  // Apple's TrueType tag is fine
  EXPECT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
}

TEST(TtfFace, ABundledCffSourceIsUnsupported) {
  const std::vector<uint8_t> data = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  ASSERT_FALSE(data.empty());
  Face face;
  EXPECT_EQ(FaceStatus::Unsupported, parse(data, &face));
}

TEST(TtfFace, BrokenFilesAreMalformed) {
  Face face;
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes{}, &face));
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes{'w', 'O', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0}, &face));
  const Bytes good = buildFont(threeGlyphs());
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes(good.begin(), good.begin() + 40), &face));  // directory cut

  Bytes data = buildFont(threeGlyphs());
  data[findRecord(data, "hmtx")] = 'x';  // the table is gone
  EXPECT_EQ(FaceStatus::Malformed, parse(data, &face));
}

TEST(TtfFace, ImpossibleHeaderValuesAreMalformed) {
  Face face;
  Font font = threeGlyphs();
  font.unitsPerEm = 8;
  EXPECT_EQ(FaceStatus::Malformed, parse(buildFont(font), &face));
  Bytes data = buildFont(threeGlyphs());
  set32(data, findRecord(data, "glyf") + 12, static_cast<uint32_t>(data.size()));  // glyf runs past the end
  EXPECT_EQ(FaceStatus::Malformed, parse(data, &face));
}

TEST(TtfFace, Format4MapsByDeltasAndByTheGlyphArray) {
  Font font = threeGlyphs();
  font.cmap = {{'A', 1}, {'B', 2}, {'C', 1}, {0x20AC, 2}};
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(2, lookup(buildFont(font), 'B'));
  EXPECT_EQ(1, lookup(buildFont(font), 'C'));
  EXPECT_EQ(2, lookup(buildFont(font), 0x20AC));
  EXPECT_EQ(0, lookup(buildFont(font), 'D'));
  EXPECT_EQ(0, lookup(buildFont(font), 0x1F600));  // past the BMP: never in format 4
  font.cmapRangeOffsets = true;
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(2, lookup(buildFont(font), 'B'));
  EXPECT_EQ(1, lookup(buildFont(font), 'C'));
  EXPECT_EQ(2, lookup(buildFont(font), 0x20AC));
  EXPECT_EQ(0, lookup(buildFont(font), 'D'));
}

TEST(TtfFace, Format12ReachesPastTheBmp) {
  Font font = threeGlyphs();
  font.cmapFormat12 = true;
  font.cmap = {{'A', 1}, {'B', 2}, {0x1F600, 1}, {0x1F601, 2}};
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  EXPECT_EQ(12, face.cmapFormat);
  EXPECT_EQ(1, lookup(data, 'A'));
  EXPECT_EQ(2, lookup(data, 0x1F601));
  EXPECT_EQ(0, lookup(data, 0x1F602));
}

TEST(TtfFace, AGlyphPastNumGlyphsReadsAsNone) {
  Font font = threeGlyphs();
  font.cmap = {{'A', 1}, {'Z', 7}};
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(0, lookup(buildFont(font), 'Z'));
}

TEST(TtfFace, TheCmapIsChosenAsFreeTypeChoosesIt) {
  // From the last record backwards: a UCS-4 map first, else any Unicode map; formats it cannot read are
  // passed over, and Mac Roman is not Unicode.
  const Bytes mapsA1 = cmapFormat4({{'A', 1}}, false);
  const Bytes mapsA2 = cmapFormat4({{'A', 2}}, false);
  const Bytes format12A2 = cmapFormat12({{'A', 2}});
  Bytes format6(10, 0);
  set16(format6, 0, 6);
  struct Case {
    const char* what;
    std::vector<std::tuple<uint16_t, uint16_t, Bytes>> subtables;
    int expected;  // the glyph for 'A', or -1 for "no usable map"
  };
  const Case cases[] = {
      {"the last Unicode BMP map wins", {{0, 3, mapsA1}, {3, 1, mapsA2}}, 2},
      {"a UCS-4 map wins over a later BMP map", {{3, 10, format12A2}, {3, 1, mapsA1}}, 2},
      {"(0,4) is UCS-4 too", {{0, 4, format12A2}, {3, 1, mapsA1}}, 2},
      {"a format it cannot read is passed over", {{3, 1, mapsA2}, {3, 10, format6}}, 2},
      {"variation sequences are not a map", {{3, 1, mapsA1}, {0, 5, mapsA2}}, 1},
      {"Mac Roman alone is no Unicode map", {{1, 0, mapsA1}}, -1},
  };
  for (const Case& c : cases) {
    Font font = threeGlyphs();
    font.extraTables = {{"cmap", cmapTable(c.subtables)}};
    const Bytes data = buildFont(font);
    Face face;
    if (c.expected < 0) {
      EXPECT_EQ(FaceStatus::Unsupported, parse(data, &face)) << c.what;
    } else {
      EXPECT_EQ(c.expected, lookup(data, 'A')) << c.what;
    }
  }
}

TEST(TtfFace, HmtxIsReadAsFreeTypeReadsIt) {
  Font font = threeGlyphs();
  font.numHMetrics = 2;  // glyph 2 shares glyph 1's advance and keeps its own left side bearing
  font.glyphs[2].lsbIsXMin = false;
  font.glyphs[2].lsb = 33;
  const Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  uint16_t advance = 0;
  int16_t lsb = 0;
  horizontalMetrics(bytes, face, 1, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(20, lsb);
  horizontalMetrics(bytes, face, 2, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(33, lsb);
  // Past the table, both read 0 rather than anything outside it.
  face.hmtxLength = 8;
  horizontalMetrics(bytes, face, 2, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(0, lsb);
  face.hmtxLength = 2;
  horizontalMetrics(bytes, face, 1, &advance, &lsb);
  EXPECT_EQ(0, advance);
  EXPECT_FALSE(bytes.outOfRange());
}

TEST(TtfFace, PointSizesBecomeFreeTypesPixelsPerEm) {
  EXPECT_EQ(1600u, ppemFromPoints(12));
  EXPECT_EQ(1867u, ppemFromPoints(14));
  EXPECT_EQ(2400u, ppemFromPoints(18));
}

TEST(TtfFace, AdvancesAreTheConvertersTwelveFour) {
  const Bytes data = buildFont(threeGlyphs());
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(122356, unitsToPixels(face, 1867));
  EXPECT_EQ(233, advance12_4(bytes, face, 0, 1867));  // 500 units: 955906 in 16.16, 14.59 px
  EXPECT_EQ(299, advance12_4(bytes, face, 1, 1867));  // 640 units: 1223560 in 16.16
  EXPECT_EQ(80, advance12_4(bytes, face, 0, 640));    // 500 units at 10 px: 4.99998 px rounds to 5
}

TEST(TtfFace, LineMetricsAreTheConverters) {
  Font font = threeGlyphs();
  font.ascender = 1057;  // Bookerly's hhea
  font.descender = -261;
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  const LineMetrics m = lineMetrics(face, 1867);  // FreeType: height 2432, ascender 1984, descender -512
  EXPECT_EQ(38, m.advanceY);
  EXPECT_EQ(31, m.ascender);
  EXPECT_EQ(-8, m.descender);
}

TEST(TtfFace, Os2MetricsAreUsedWhereFreeTypeUsesThem) {
  Font font = threeGlyphs();
  font.os2 = true;
  font.typoAscender = 900;
  font.typoDescender = -300;
  font.typoLineGap = 100;
  font.winAscent = 1100;
  font.winDescent = 400;
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(800, face.ascender);  // hhea, while OS/2 does not ask for its typo metrics

  font.os2Selection = 0x80;  // USE_TYPO_METRICS
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(900, face.ascender);
  EXPECT_EQ(-300, face.descender);
  EXPECT_EQ(1300, face.height);

  font.os2Selection = 0;
  font.ascender = 0;  // hhea empty: OS/2's typo metrics
  font.descender = 0;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(900, face.ascender);

  font.typoAscender = 0;  // and those empty too: the win metrics
  font.typoDescender = 0;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(1100, face.ascender);
  EXPECT_EQ(-400, face.descender);
  EXPECT_EQ(1500, face.height);
}

TEST(TtfFace, HugeAdvancesNeitherOverflowNorWrap) {
  // A lying hmtx on a tiny em: the 16.16 linear advance passes 2^31, and the 12.4 result passes 16 bits.
  Font font = threeGlyphs();
  font.unitsPerEm = 16;
  font.glyphs[1].advance = 63550;
  Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(524288u, advance12_4(bytes, face, 1, 528));  // 2147481600 in 16.16: the + 2048 used to overflow

  font = threeGlyphs();
  font.unitsPerEm = 2048;
  font.glyphs[1].advance = 65535;
  data = buildFont(font);
  bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(130558u, advance12_4(bytes, face, 1, 255 << 6));  // 8,160 px: wrapped to 4,064 px in 16 bits
}

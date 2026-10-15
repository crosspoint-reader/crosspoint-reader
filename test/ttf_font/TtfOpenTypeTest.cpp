// Kerning and ligatures for the TTF engine (TtfOpenType): the glue over the SDK's FreeInkFont layout readers, which
// test the OpenType rules themselves (libs/font/FreeInkFont/test/host). Here: that a style finds and resolves its
// tables, which table it kerns from, and the ligatures EpdFont applies, on built fonts and on Bookerly.
#include <Gpos.h>
#include <Gsub.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "TtfBuilder.h"
#include "TtfFontTestUtil.h"
#include "TtfLayoutBuilder.h"
#include "TtfOpenType.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {

// A font with glyphs 1..n for 'A'..: every glyph a box, so only the layout tables differ between tests.
Font boxes(const uint16_t n) {
  Font font;
  font.glyphs.resize(n + 1);
  for (uint16_t g = 0; g <= n; ++g) font.glyphs[g].contours = {rect(0, 0, 400, 600)};
  for (uint16_t g = 1; g <= n; ++g) font.cmap[static_cast<uint32_t>('A' + g - 1)] = g;
  return font;
}

struct Parsed {
  std::vector<uint8_t> file;
  FontBytes bytes;
  ttf::Face face;
  explicit Parsed(const Font& font) : file(buildFont(font)), bytes(file.data(), static_cast<uint32_t>(file.size())) {
    EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  }
};

// A GPOS whose 'latn' kern feature kerns 1-2 by -50 and 1-3 by 30.
Bytes kernedGpos() {
  LayoutLookup lookup;
  PairSubtable pairs;
  pairs.pairs = {{1, {{2, -50}, {3, 30}}}};
  lookup.pairSubtables = {pairs};
  return layoutTable({{"latn", {0}}}, {{"kern", {0}}}, {lookup});
}

}  // namespace

TEST(TtfLayoutTables, TheFaceFindsTheLayoutTables) {
  Font font = boxes(4);
  font.extraTables = {{"GPOS", kernedGpos()}, {"kern", legacyKernTable({{{1, 2}, -40}})}};
  Parsed p(font);
  EXPECT_NE(0u, p.face.gpos);
  EXPECT_NE(0u, p.face.kern);
  EXPECT_EQ(0u, p.face.gsub);
}

TEST(TtfKerning, GposKernsThroughTheResolvedSubtables) {
  Font font = boxes(4);
  font.extraTables = {{"GPOS", kernedGpos()}};
  Parsed p(font);
  ttf::Kerning kerning;
  ASSERT_TRUE(kerning.resolve(p.bytes, p.face));
  EXPECT_EQ(1u, kerning.subtableCount());
  EXPECT_FALSE(kerning.legacy());
  bool covered = false;
  EXPECT_EQ(-50, kerning.pair(p.bytes, p.face, 1, 2, &covered));
  EXPECT_TRUE(covered);
  EXPECT_EQ(30, kerning.pair(p.bytes, p.face, 1, 3));
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 1, 4, &covered));
  EXPECT_TRUE(covered);  // 1 kerns, just not with 4: worth remembering as 0
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 3, 1, &covered));
  EXPECT_FALSE(covered);  // 3 kerns with nothing
}

TEST(TtfKerning, WithoutGposTheLegacyKernTableKerns) {
  Font font = boxes(4);
  font.extraTables = {{"kern", legacyKernTable({{{1, 2}, -40}, {{1, 4}, 15}, {{3, 1}, -5}})}};
  Parsed p(font);
  ttf::Kerning kerning;
  ASSERT_TRUE(kerning.resolve(p.bytes, p.face));
  EXPECT_TRUE(kerning.legacy());
  bool covered = false;
  EXPECT_EQ(-40, kerning.pair(p.bytes, p.face, 1, 2, &covered));
  EXPECT_TRUE(covered);
  EXPECT_EQ(15, kerning.pair(p.bytes, p.face, 1, 4));
  EXPECT_EQ(-5, kerning.pair(p.bytes, p.face, 3, 1));
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 2, 1, &covered));
  EXPECT_FALSE(covered);
}

TEST(TtfKerning, GposWinsOverTheLegacyTable) {
  Font font = boxes(4);
  font.extraTables = {{"GPOS", kernedGpos()}, {"kern", legacyKernTable({{{1, 2}, -40}})}};
  Parsed p(font);
  ttf::Kerning kerning;
  ASSERT_TRUE(kerning.resolve(p.bytes, p.face));
  EXPECT_FALSE(kerning.legacy());
  EXPECT_EQ(-50, kerning.pair(p.bytes, p.face, 1, 2));
}

TEST(TtfKerning, AFontWithoutKerningResolvesNothing) {
  Parsed p(boxes(4));
  ttf::Kerning kerning;
  EXPECT_FALSE(kerning.resolve(p.bytes, p.face));
  EXPECT_FALSE(kerning.any());
  bool covered = true;
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 1, 2, &covered));
  EXPECT_FALSE(covered);
}

TEST(TtfKerning, ABrokenGposKernsNothing) {
  Font font = boxes(4);
  Bytes gpos = kernedGpos();
  set16(gpos, 8, 0xFFF0);  // the lookup list points past the table
  font.extraTables = {{"GPOS", gpos}};
  Parsed p(font);
  ttf::Kerning kerning;
  EXPECT_FALSE(kerning.resolve(p.bytes, p.face));
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 1, 2));
}

TEST(TtfKerning, BookerlyResolvedAgreesWithTheStatelessReader) {
  // The resolved subtables must give what the SDK's per-call walk gives: every pair of printable ASCII in each of
  // the four styles.
  for (const char* name : {"Bookerly/Bookerly-Regular.ttf", "Bookerly/Bookerly-Bold.ttf",
                           "Bookerly/Bookerly-Italic.ttf", "Bookerly/Bookerly-BoldItalic.ttf"}) {
    const std::vector<uint8_t> file = readFont(name);
    FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
    ttf::Face face;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
    ttf::Kerning kerning;
    ASSERT_TRUE(kerning.resolve(bytes, face)) << name;
    int kerned = 0;
    for (uint32_t l = 0x21; l < 0x7F; ++l) {
      for (uint32_t r = 0x21; r < 0x7F; ++r) {
        const uint16_t gl = ttf::glyphIndex(bytes, face, l);
        const uint16_t gr = ttf::glyphIndex(bytes, face, r);
        const int32_t expected =
            freeink::font::gpos::PairKernAdjustment(file.data() + face.gpos, face.gposLength, gl, gr);
        ASSERT_EQ(expected, kerning.pair(bytes, face, gl, gr)) << name << " " << char(l) << char(r);
        kerned += expected != 0;
      }
    }
    EXPECT_GT(kerned, 500) << name;  // a real kerning table, not a vacuous match
  }
}

// --- Ligatures ---

TEST(TtfLigatures, TheFontsFfiLigaturesBecomeEpdFontPairs) {
  Font font = boxes(10);
  font.cmap['f'] = 1;
  font.cmap['i'] = 2;
  font.cmap['l'] = 3;
  LayoutLookup liga;
  liga.type = 4;
  liga.ligatures = {{{1, 1, 2}, 7}, {{1, 1, 3}, 8}, {{1, 1}, 4}, {{1, 2}, 5}, {{1, 3}, 6}};  // ffi ffl ff fi fl
  font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)}};
  Parsed p(font);
  ttf::Ligatures ligatures;
  ASSERT_TRUE(ttf::resolveLigatures(p.bytes, p.face, &ligatures));
  EXPECT_EQ(5, ligatures.count);
  const uint16_t expectedGlyphs[5] = {4, 5, 6, 7, 8};  // ff fi fl ffi ffl
  for (int k = 0; k < 5; ++k) EXPECT_EQ(expectedGlyphs[k], ligatures.glyphs[k]) << k;
  // Sorted as EpdFont searches them; ffi and ffl chain through ff.
  EXPECT_EQ(('f' << 16 | 'f'), ligatures.pairs[0].pair);
  EXPECT_EQ(0xFB00u, ligatures.pairs[0].ligatureCp);
  EXPECT_EQ((0xFB00u << 16 | 'i'), ligatures.pairs[3].pair);
  EXPECT_EQ(0xFB03u, ligatures.pairs[3].ligatureCp);
  for (int k = 1; k < 5; ++k) EXPECT_LT(ligatures.pairs[k - 1].pair, ligatures.pairs[k].pair);
  EXPECT_EQ(5, ligatures.glyphFor(p.bytes, p.face, 0xFB01));  // the GSUB glyph, whatever the cmap says
  EXPECT_EQ(2, ligatures.glyphFor(p.bytes, p.face, 'i'));
}

TEST(TtfLigatures, WithoutAnFfLigatureFfiDoesNotChain) {
  Font font = boxes(10);
  font.cmap['f'] = 1;
  font.cmap['i'] = 2;
  LayoutLookup liga;
  liga.type = 4;
  liga.ligatures = {{{1, 1, 2}, 7}, {{1, 2}, 5}};  // ffi and fi, no ff
  font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)}};
  Parsed p(font);
  ttf::Ligatures ligatures;
  ASSERT_TRUE(ttf::resolveLigatures(p.bytes, p.face, &ligatures));
  EXPECT_EQ(1, ligatures.count);  // fi: "ffi" then draws f + fi
  EXPECT_EQ(0xFB01u, ligatures.pairs[0].ligatureCp);
}

TEST(TtfLigatures, ALigatureGlyphTheFontLacksIsLeftOut) {
  // GSUB names glyph 99 in a font of 11 glyphs: the rasterizer could not draw it, so every such ligature would draw
  // as missing. Left out, the text draws its letters instead.
  // Resolves ff and fi as given, and checks that `leftOut` (U+FB00-FB04) draws the cmap's glyph: none.
  const auto resolve = [](const uint16_t ff, const uint16_t fi, const uint32_t leftOut, ttf::Ligatures* ligatures) {
    Font font = boxes(10);
    font.cmap['f'] = 1;
    font.cmap['i'] = 2;
    font.cmap['l'] = 3;
    LayoutLookup liga;
    liga.type = 4;
    liga.ligatures = {{{1, 1, 2}, 7}, {{1, 1, 3}, 8}, {{1, 1}, ff}, {{1, 2}, fi}, {{1, 3}, 6}};  // ffi ffl ff fi fl
    font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)}};
    Parsed p(font);
    ASSERT_EQ(11, p.face.numGlyphs);
    ttf::resolveLigatures(p.bytes, p.face, ligatures);
    EXPECT_EQ(0, ligatures->glyphFor(p.bytes, p.face, leftOut));
  };
  const auto hasPair = [](const ttf::Ligatures& ligatures, const uint32_t ligatureCp) {
    for (uint8_t k = 0; k < ligatures.count; ++k) {
      if (ligatures.pairs[k].ligatureCp == ligatureCp) return true;
    }
    return false;
  };

  ttf::Ligatures noFi;
  resolve(/*ff=*/4, /*fi=*/99, 0xFB01, &noFi);
  EXPECT_EQ(0, noFi.glyphs[1]);
  EXPECT_FALSE(hasPair(noFi, 0xFB01));
  EXPECT_EQ(4, noFi.count);  // ff, fl, and ffi and ffl through ff
  EXPECT_EQ(7, noFi.glyphs[3]);
  EXPECT_TRUE(hasPair(noFi, 0xFB03));

  ttf::Ligatures noFf;
  resolve(/*ff=*/99, /*fi=*/5, 0xFB00, &noFf);
  EXPECT_EQ(0, noFf.glyphs[0]);
  EXPECT_EQ(2, noFf.count);  // fi and fl: ffi and ffl cannot chain through a missing ff
  EXPECT_FALSE(hasPair(noFf, 0xFB00));
  EXPECT_FALSE(hasPair(noFf, 0xFB03));
  EXPECT_FALSE(hasPair(noFf, 0xFB04));
}

TEST(TtfLigatures, AFontWithoutGsubHasNone) {
  Parsed p(boxes(4));
  ttf::Ligatures ligatures;
  EXPECT_FALSE(ttf::resolveLigatures(p.bytes, p.face, &ligatures));
  EXPECT_EQ(0, ligatures.count);
}

TEST(TtfLigatures, BookerlyHasTheFiAndFlLigatures) {
  for (const char* name : {"Bookerly/Bookerly-Regular.ttf", "Bookerly/Bookerly-Bold.ttf",
                           "Bookerly/Bookerly-Italic.ttf", "Bookerly/Bookerly-BoldItalic.ttf"}) {
    const std::vector<uint8_t> file = readFont(name);
    FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
    ttf::Face face;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
    ttf::Ligatures ligatures;
    ASSERT_TRUE(ttf::resolveLigatures(bytes, face, &ligatures)) << name;
    EXPECT_NE(0, ligatures.glyphs[1]) << name << " fi";
    EXPECT_NE(0, ligatures.glyphs[2]) << name << " fl";
  }
}

// --- Hostile and truncated tables ---

TEST(TtfKerning, AStyleKeepsAtMostTheSubtableCap) {
  // 76 PairPos subtables more than the cap, in one lookup. The first kern 1-2; the last one alone kerns 5-6.
  const uint16_t total = ttf::Kerning::kMaxSubtables + 76;
  LayoutLookup lookup;
  for (uint16_t s = 0; s < total; ++s) {
    PairSubtable pairs;
    if (s + 1 == total) {
      pairs.pairs = {{5, {{6, 77}}}};
    } else {
      pairs.pairs = {{1, {{2, -50}}}};
    }
    lookup.pairSubtables.push_back(pairs);
  }
  Font font = boxes(6);
  font.extraTables = {{"GPOS", layoutTable({{"latn", {0}}}, {{"kern", {0}}}, {lookup})}};
  Parsed p(font);
  // The font has them all, more than the cap.
  const unsigned found =
      freeink::font::gpos::ResolveKernSubtables(p.file.data() + p.face.gpos, p.face.gposLength, nullptr, 0);
  ASSERT_EQ(total, found);
  ttf::Kerning kerning;
  ASSERT_TRUE(kerning.resolve(p.bytes, p.face));
  EXPECT_EQ(ttf::Kerning::kMaxSubtables, kerning.subtableCount());
  EXPECT_EQ(-50, kerning.pair(p.bytes, p.face, 1, 2));
  // The subtable past the cap is dropped, so its pair kerns 0: the first ones are kept.
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 5, 6));
}

TEST(TtfKerning, ATruncatedGposKernsSafely) {
  Font font = boxes(4);
  font.extraTables = {{"GPOS", kernedGpos()}};
  Parsed p(font);
  const uint32_t full = p.face.gposLength;
  ASSERT_GT(full, 0u);
  for (uint32_t length = 0; length <= full; ++length) {
    ttf::Face cut = p.face;
    cut.gposLength = length;
    ttf::Kerning kerning;
    const bool resolved = kerning.resolve(p.bytes, cut);
    const int32_t a = kerning.pair(p.bytes, cut, 1, 2);
    const int32_t b = kerning.pair(p.bytes, cut, 1, 3);
    const int32_t c = kerning.pair(p.bytes, cut, 3, 1);
    if (length == full) {
      EXPECT_TRUE(resolved);
      EXPECT_EQ(-50, a);
      EXPECT_EQ(30, b);
      EXPECT_EQ(0, c);
    }
  }
}

TEST(TtfLigatures, ABrokenGsubLigatesSafely) {
  Font font = boxes(10);
  font.cmap['f'] = 1;
  font.cmap['i'] = 2;
  font.cmap['l'] = 3;
  LayoutLookup liga;
  liga.type = 4;
  liga.ligatures = {{{1, 1, 2}, 7}, {{1, 1, 3}, 8}, {{1, 1}, 4}, {{1, 2}, 5}, {{1, 3}, 6}};
  font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)}};
  Parsed p(font);
  ASSERT_NE(0u, p.face.gsub);
  const auto check = [&](const ttf::Face& face, const char* what, const uint32_t at) {
    ttf::Ligatures ligatures;
    ttf::resolveLigatures(p.bytes, face, &ligatures);
    ASSERT_LE(ligatures.count, 5) << what << " " << at;
    for (uint8_t k = 0; k < ligatures.count; ++k) {
      ASSERT_NE(0, ligatures.glyphs[ligatures.pairs[k].ligatureCp - ttf::Ligatures::kFirst]) << what << " " << at;
      if (k != 0) {
        ASSERT_LT(ligatures.pairs[k - 1].pair, ligatures.pairs[k].pair) << what << " " << at;
      }
    }
  };
  const uint32_t full = p.face.gsubLength;
  for (uint32_t length = 0; length <= full; ++length) {
    ttf::Face cut = p.face;
    cut.gsubLength = length;
    check(cut, "truncated", length);
  }
  for (uint32_t i = 0; i < full; ++i) {
    p.file[p.face.gsub + i] ^= 0xFF;
    check(p.face, "corrupted", i);
    p.file[p.face.gsub + i] ^= 0xFF;
  }
}

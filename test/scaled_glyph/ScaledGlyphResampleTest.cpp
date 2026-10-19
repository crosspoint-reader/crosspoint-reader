// Shrinking a glyph (emitScaledGlyphPixels at a scale below 1): text the book sets smaller than the body, small
// caps, superscripts. Nearest-neighbour point sampling skipped whole source rows and columns (a 0.75 shrink drops
// every fourth), so a one-pixel stroke on a skipped row vanished and stems came out uneven, and it emitted no
// anti-aliasing levels at all. The glyph is integrated over the area each destination pixel covers instead, as
// enlarging already does, and re-quantised into the font's four ink levels.
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ScaledGlyphResample.h"

namespace {

// A 2-bit glyph from rows of '0'..'3' (ink levels), packed as the fonts store them: four pixels a byte, the first in
// the high bits, rows back to back.
struct Glyph {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> bits;
};

Glyph glyphOf(const std::vector<std::string>& rows) {
  Glyph g;
  g.h = static_cast<int>(rows.size());
  g.w = static_cast<int>(rows[0].size());
  g.bits.assign((g.w * g.h + 3) / 4, 0);
  for (int y = 0; y < g.h; ++y) {
    for (int x = 0; x < g.w; ++x) {
      const int pos = y * g.w + x;
      g.bits[pos >> 2] |= static_cast<uint8_t>((rows[y][x] - '0') << ((3 - (pos & 3)) * 2));
    }
  }
  return g;
}

using Pixels = std::set<std::pair<int, int>>;

// The destination pixels whose ink level is in `drawMask` (bit n: level n), shrunk to `scale`.
Pixels shrink(const Glyph& g, const float scale, const uint8_t drawMask) {
  const int dstW = static_cast<int>(g.w * scale + 0.5f);
  const int dstH = static_cast<int>(g.h * scale + 0.5f);
  Pixels out;
  emitScaledGlyphPixels(g.bits.data(), true, g.w, g.h, dstW, dstH, scale, drawMask,
                        [&out](const int x, const int y) { out.insert({x, y}); });
  return out;
}

constexpr uint8_t kAnyInk = 0x0E;   // levels 1-3: what the black-and-white pass draws
constexpr uint8_t kGreys = 0x06;    // levels 1-2
constexpr uint8_t kFullInk = 0x08;  // level 3

}  // namespace

TEST(ScaledGlyphShrink, AOnePixelStrokeSurvives) {
  // A crossbar on source row 4 of 12. At 0.75 point sampling reads rows 0, 1, 2, 3, 5, 6, 7, 9, 10: never 4 or 8.
  std::vector<std::string> rows(12, "00000000");
  rows[4] = "33333333";
  const Pixels drawn = shrink(glyphOf(rows), 0.75f, kAnyInk);
  EXPECT_FALSE(drawn.empty()) << "the stroke vanished";
}

TEST(ScaledGlyphShrink, AnEdgeAcrossAPixelIsGrey) {
  // A stem three pixels wide, shrunk to 0.75: destination columns 0 and 1 lie inside it, column 2 covers a quarter
  // of stem (source x 2.67 to 4), so it is a grey edge, not black and not missing.
  const Glyph stem = glyphOf(std::vector<std::string>(8, "33300000"));
  const Pixels black = shrink(stem, 0.75f, kFullInk);
  const Pixels grey = shrink(stem, 0.75f, kGreys);
  ASSERT_FALSE(black.empty());
  ASSERT_FALSE(grey.empty()) << "no anti-aliasing levels";
  for (const auto& [x, y] : black) EXPECT_LT(x, 2) << "black at column " << x;
  for (const auto& [x, y] : grey) EXPECT_EQ(x, 2) << "grey at column " << x;
}

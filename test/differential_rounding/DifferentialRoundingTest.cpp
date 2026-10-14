#include <gtest/gtest.h>

#include <cmath>

#include "lib/EpdFont/EpdFont.h"
#include "lib/EpdFont/EpdFontData.h"
#include "lib/EpdFont/SmallCaps.h"

// ============================================================================
// Synthetic test font
//
// Glyphs: 'T' (0x54), 'a' (0x61), 'o' (0x6F), 'x' (0x78)
//   - 'x' advance is 136 FP (8.5px) -- frac = 8, exactly at the rounding
//     boundary where absolute vs differential snapping diverges for "oo".
//   - No U+FFFD replacement glyph, so unknown codepoints trigger the
//     null-glyph path in getTextBounds.
//
// Kern pairs (4.4 fixed-point):
//   T->a: -5  (-0.3125px)     T->o: -7  (-0.4375px)
//   o->a: -2  (-0.125px)      o->o: -3  (-0.1875px)
// ============================================================================

namespace {

// clang-format off
const EpdGlyph kGlyphs[] = {
  // idx  width  height  advanceX  left  top  dataLength  dataOffset
  /* 0 'T' */ { 8, 12, 137, 0, 12, 0, 0 },
  /* 1 'a' */ { 7,  8, 130, 0,  8, 0, 0 },
  /* 2 'o' */ { 8,  8, 145, 0,  8, 0, 0 },
  /* 3 'x' */ { 7,  8, 136, 0,  8, 0, 0 },
};

const EpdUnicodeInterval kIntervals[] = {
  { 0x54, 0x54, 0 },  // 'T' -> glyph[0]
  { 0x61, 0x61, 1 },  // 'a' -> glyph[1]
  { 0x6F, 0x6F, 2 },  // 'o' -> glyph[2]
  { 0x74, 0x74, 0 },  // 't' -> glyph[0] ('T'): lets small-caps fold t->T to a real glyph
  { 0x78, 0x78, 3 },  // 'x' -> glyph[3]
};

const EpdKernClassEntry kKernLeft[] = {
  { 0x54, 1 },  // 'T' -> left class 1
  { 0x6F, 2 },  // 'o' -> left class 2
};

const EpdKernClassEntry kKernRight[] = {
  { 0x61, 1 },  // 'a' -> right class 1
  { 0x6F, 2 },  // 'o' -> right class 2
};

// Flat matrix: leftClassCount(2) x rightClassCount(2), 4.4 fixed-point
//   [L1,R1]=kern(T,a)  [L1,R2]=kern(T,o)  [L2,R1]=kern(o,a)  [L2,R2]=kern(o,o)
const int8_t kKernMatrix[] = { -5, -7, -2, -3 };

const EpdFontData kTestFontData = {
  .bitmap            = nullptr,
  .glyph             = kGlyphs,
  .intervals         = kIntervals,
  .intervalCount     = 5,
  .advanceY          = 16,
  .ascender          = 12,
  .descender         = 0,
  .is2Bit            = false,
  .groups            = nullptr,
  .groupCount        = 0,
  .glyphToGroup      = nullptr,
  .kernLeftClasses   = kKernLeft,
  .kernRightClasses  = kKernRight,
  .kernMatrix        = kKernMatrix,
  .kernLeftEntryCount  = 2,
  .kernRightEntryCount = 2,
  .kernLeftClassCount  = 2,
  .kernRightClassCount = 2,
  .ligaturePairs     = nullptr,
  .ligaturePairCount = 0,
  .provider          = nullptr,
};
// clang-format on

EpdFont& testFont() {
  static EpdFont font(&kTestFontData);
  return font;
}

int textWidth(const char* str) {
  int w = 0, h = 0;
  testFont().getTextDimensions(str, &w, &h);
  return w;
}

int textHeight(const char* str) {
  int w = 0, h = 0;
  testFont().getTextDimensions(str, &w, &h);
  return h;
}

int textWidthSC(const char* str) {
  int w = 0, h = 0;
  testFont().getTextDimensions(str, &w, &h, /*useSmallCaps=*/true);
  return w;
}

}  // namespace

// ============================================================================
// Part 1: Pure fp4 math tests
// ============================================================================

TEST(Fp4Math, RoundTripIntegerPixels) {
  for (int px = 0; px < 500; px++) {
    EXPECT_EQ(fp4::toPixel(fp4::fromPixel(px)), px) << "px=" << px;
  }
}

TEST(Fp4Math, RoundingBoundaries) {
  EXPECT_EQ(fp4::toPixel(0), 0);
  EXPECT_EQ(fp4::toPixel(7), 0);    // 0.4375 -> 0
  EXPECT_EQ(fp4::toPixel(8), 1);    // 0.5 -> 1 (round half up)
  EXPECT_EQ(fp4::toPixel(15), 1);   // 0.9375 -> 1
  EXPECT_EQ(fp4::toPixel(16), 1);   // 1.0 -> 1
  EXPECT_EQ(fp4::toPixel(24), 2);   // 1.5 -> 2
  EXPECT_EQ(fp4::toPixel(-8), 0);   // -0.5 -> 0
  EXPECT_EQ(fp4::toPixel(-9), -1);  // -0.5625 -> -1
  EXPECT_EQ(fp4::toPixel(-16), -1);

  EXPECT_EQ(fp4::toPixel(137 + (-9)), 8);  // 128 = 8.0 exact
  EXPECT_EQ(fp4::toPixel(137 + (-5)), 8);  // 132 = 8.25
  EXPECT_EQ(fp4::toPixel(137 + (-1)), 9);  // 136 = 8.5 (half rounds up)

  // Every 4.4 kern (-128..127) on every 8.x px advance (128..143): the step is always the
  // nearest pixel, halves rounding up.
  for (int32_t advance = 128; advance < 144; advance++) {
    for (int32_t kern = -128; kern <= 127; kern++) {
      const int32_t fp = advance + kern;
      EXPECT_EQ(fp4::toPixel(fp), static_cast<int>(std::floor(fp4::toFloat(fp) + 0.5f)))
          << "advance=" << advance << " kern=" << kern;
    }
  }
}

// ============================================================================
// Part 2: Integration tests using real EpdFont::getTextDimensions
// ============================================================================

TEST(EpdFont, KernLookup) {
  EXPECT_EQ(testFont().getKerning('T', 'a'), -5);
  EXPECT_EQ(testFont().getKerning('T', 'o'), -7);
  EXPECT_EQ(testFont().getKerning('o', 'a'), -2);
  EXPECT_EQ(testFont().getKerning('o', 'o'), -3);
  EXPECT_EQ(testFont().getKerning('a', 'o'), 0);  // 'a' has no left class
  EXPECT_EQ(testFont().getKerning('x', 'o'), 0);  // 'x' has no left class
  EXPECT_EQ(testFont().getKerning('T', 'x'), 0);  // 'x' has no right class
  EXPECT_EQ(testFont().getKerning('T', 'T'), 0);  // 'T' has no right class
}

// The font above is the DENSE representation, which only SD-card fonts use now. Every built-in
// font ships the sparse (CSR) form instead, so it needs its own coverage: same pairs, same
// answers, including the two zero cells that the sparse form stores by omitting them.
namespace {
// Dense equivalent, for reference:  [L1,R1]=-5  [L1,R2]=-7  [L2,R1]=-2  [L2,R2]=-3
// Rows are [rowOffsets[l], rowOffsets[l+1]) and sorted ascending by column.
const uint16_t kKernRowOffsetsFull[] = {0, 2, 4};
const uint8_t kKernSparseColsFull[] = {0, 1, 0, 1};
const int8_t kKernSparseValuesFull[] = {-5, -7, -2, -3};

// A row with a hole: left class 1 keeps only its R2 entry, left class 2 only its R1 entry.
// This is the case the dense form cannot distinguish from a stored zero, and the one a
// binary search gets wrong if it does not confirm the key it landed on.
const uint16_t kKernRowOffsetsSparse[] = {0, 1, 2};
const uint8_t kKernSparseColsSparse[] = {1, 0};
const int8_t kKernSparseValuesSparse[] = {-7, -2};

EpdFontData makeSparseFontData(const uint16_t* rowOffsets, const uint8_t* cols, const int8_t* values) {
  EpdFontData d = kTestFontData;
  d.kernMatrix = nullptr;  // force the sparse path
  d.kernRowOffsets = rowOffsets;
  d.kernSparseCols = cols;
  d.kernSparseValues = values;
  return d;
}
}  // namespace

TEST(EpdFont, KernLookupSparseMatchesDense) {
  const EpdFontData sparseData = makeSparseFontData(kKernRowOffsetsFull, kKernSparseColsFull, kKernSparseValuesFull);
  EpdFont sparse(&sparseData);

  // Every pair the dense font answers, the sparse font must answer identically.
  for (const uint32_t left : {'T', 'a', 'o', 'x'}) {
    for (const uint32_t right : {'T', 'a', 'o', 'x'}) {
      EXPECT_EQ(sparse.getKerning(left, right), testFont().getKerning(left, right))
          << "pair " << static_cast<char>(left) << static_cast<char>(right);
    }
  }
}

TEST(EpdFont, KernLookupSparseHandlesMissingEntries) {
  const EpdFontData sparseData =
      makeSparseFontData(kKernRowOffsetsSparse, kKernSparseColsSparse, kKernSparseValuesSparse);
  EpdFont sparse(&sparseData);

  EXPECT_EQ(sparse.getKerning('T', 'o'), -7);  // present, and not the first column of its row
  EXPECT_EQ(sparse.getKerning('o', 'a'), -2);  // present, first column of its row
  EXPECT_EQ(sparse.getKerning('T', 'a'), 0);   // absent: column below the one stored
  EXPECT_EQ(sparse.getKerning('o', 'o'), 0);   // absent: column above the one stored
  EXPECT_EQ(sparse.getKerning('a', 'o'), 0);   // no left class at all
  EXPECT_EQ(sparse.getKerning('T', 'x'), 0);   // no right class at all
}

// The class maps above are the PACKED form, which only SD-card fonts use now. Built-in fonts
// ship the split arrays, so the same coexistence check the matrix gets applies here too.
namespace {
const uint16_t kKernLeftCps[] = {0x54, 0x6F};
const uint8_t kKernLeftIds[] = {1, 2};
const uint16_t kKernRightCps[] = {0x61, 0x6F};
const uint8_t kKernRightIds[] = {1, 2};

EpdFontData makeSplitClassFontData() {
  EpdFontData d = kTestFontData;
  d.kernLeftClasses = nullptr;  // force the split path
  d.kernRightClasses = nullptr;
  d.kernLeftCodepoints = kKernLeftCps;
  d.kernLeftClassIds = kKernLeftIds;
  d.kernRightCodepoints = kKernRightCps;
  d.kernRightClassIds = kKernRightIds;
  return d;
}
}  // namespace

TEST(EpdFont, KernSplitClassMapMatchesPacked) {
  const EpdFontData splitData = makeSplitClassFontData();
  EpdFont split(&splitData);
  for (const uint32_t left : {'T', 'a', 'o', 'x', 'z'}) {
    for (const uint32_t right : {'T', 'a', 'o', 'x', 'z'}) {
      EXPECT_EQ(split.getKerning(left, right), testFont().getKerning(left, right))
          << "pair " << static_cast<char>(left) << static_cast<char>(right);
    }
  }
}

TEST(EpdFont, KernSplitClassMapWithSparseMatrix) {
  // The combination the built-in fonts actually ship: split class maps AND a sparse matrix.
  EpdFontData d = makeSplitClassFontData();
  d.kernMatrix = nullptr;
  d.kernRowOffsets = kKernRowOffsetsFull;
  d.kernSparseCols = kKernSparseColsFull;
  d.kernSparseValues = kKernSparseValuesFull;
  EpdFont shipped(&d);

  EXPECT_EQ(shipped.getKerning('T', 'a'), -5);
  EXPECT_EQ(shipped.getKerning('T', 'o'), -7);
  EXPECT_EQ(shipped.getKerning('o', 'a'), -2);
  EXPECT_EQ(shipped.getKerning('o', 'o'), -3);
  EXPECT_EQ(shipped.getKerning('a', 'o'), 0);  // no left class
  EXPECT_EQ(shipped.getKerning('T', 'x'), 0);  // no right class
}

TEST(EpdFont, KernLookupEmptyRowIsZero) {
  // Left class 1 stores nothing (offsets 0,0) — an empty range must not read past its row.
  static const uint16_t rowOffsets[] = {0, 0, 1};
  static const uint8_t cols[] = {0};
  static const int8_t values[] = {-2};
  const EpdFontData sparseData = makeSparseFontData(rowOffsets, cols, values);
  EpdFont sparse(&sparseData);

  EXPECT_EQ(sparse.getKerning('T', 'a'), 0);
  EXPECT_EQ(sparse.getKerning('T', 'o'), 0);
  EXPECT_EQ(sparse.getKerning('o', 'a'), -2);
}

TEST(EpdFont, GlyphLookup) {
  // getGlyph() returns a resolved value rather than a pointer -- built-in and SD-card fonts store
  // glyph records in different shapes -- so presence is `.valid` (or the explicit operator bool).
  ASSERT_TRUE(testFont().getGlyph('T').valid);
  ASSERT_TRUE(testFont().getGlyph('a').valid);
  ASSERT_TRUE(testFont().getGlyph('o').valid);
  ASSERT_TRUE(testFont().getGlyph('x').valid);
  EXPECT_EQ(testFont().getGlyph('T').advanceX, 137);
  EXPECT_EQ(testFont().getGlyph('a').advanceX, 130);
  EXPECT_EQ(testFont().getGlyph('o').advanceX, 145);
  EXPECT_EQ(testFont().getGlyph('x').advanceX, 136);

  // No U+FFFD in font, so unknown codepoints resolve to nothing
  EXPECT_FALSE(testFont().getGlyph('Z').valid);
  EXPECT_FALSE(testFont().getGlyph('b').valid);
}

// Known-value regression tests.  Expected widths are computed by hand using
// differential rounding.  If someone reverts to absolute snapping, specific
// test cases will fail.
//
// Layout trace for each string (all glyphs have left=0):
//   width = max glyph right edge = lastBaseX + glyph.width
//
// Differential step from glyph A to glyph B:
//   step = fp4::toPixel(advanceA + kern(A,B))
TEST(EpdFont, KnownWidths) {
  // "o": single glyph at x=0, width=8 -> w = 0 + 8 = 8
  EXPECT_EQ(textWidth("o"), 8);

  // "oo": step = toPixel(145 + (-3)) = toPixel(142) = 9
  //   o1 at 0, o2 at 9.  w = 9 + 8 = 17
  EXPECT_EQ(textWidth("oo"), 17);

  // "ooo": two steps of 9 -> o3 at 18, w = 18 + 8 = 26
  EXPECT_EQ(textWidth("ooo"), 26);

  // "To": step = toPixel(137 + (-7)) = 8 -> o at 8, w = 8 + 8 = 16
  EXPECT_EQ(textWidth("To"), 16);

  // "Ta": step = toPixel(137 + (-5)) = 8 -> a at 8, w = 8 + 7 = 15
  EXPECT_EQ(textWidth("Ta"), 15);

  // "oa": step = toPixel(145 + (-2)) = 9 -> a at 9, w = 9 + 7 = 16
  EXPECT_EQ(textWidth("oa"), 16);

  // "Too": T at 0, o1 at 8 (T->o step), o2 at 17 (o->o step). w = 17 + 8 = 25
  EXPECT_EQ(textWidth("Too"), 25);

  // "xo": step = toPixel(136 + 0) = 9 (no kern: x has no left class)
  //   x at 0, o at 9.  w = 9 + 8 = 17
  EXPECT_EQ(textWidth("xo"), 17);
}

// "oo" pair consistency: the pixel gap between two o's must be the same
// regardless of what prefix precedes them.  This is THE key property of
// differential rounding.  With absolute snapping, "xoo" would produce a
// different oo gap than "oo" because 'x' advance (136 FP) puts the first
// 'o' at fractional phase 8, crossing the rounding boundary differently.
TEST(EpdFont, PairConsistencyViaFont) {
  // The oo gap = width(prefix + "oo") - width(prefix + "o")
  // This isolates the pixel distance contributed by the second 'o'.
  const int oo_gap_bare = textWidth("oo") - textWidth("o");
  const int oo_gap_after_x = textWidth("xoo") - textWidth("xo");
  const int oo_gap_after_T = textWidth("Too") - textWidth("To");
  const int oo_gap_after_o = textWidth("ooo") - textWidth("oo");

  EXPECT_EQ(oo_gap_after_x, oo_gap_bare);
  EXPECT_EQ(oo_gap_after_T, oo_gap_bare);
  EXPECT_EQ(oo_gap_after_o, oo_gap_bare);
}

// Null-glyph handling: when a codepoint has no glyph (and no replacement
// glyph), the pending advance from the previous glyph must still be flushed.
// Without the flush fix, the glyph after the null would overlap the one before.
TEST(EpdFont, NullGlyphAdvancePreserved) {
  // 'Z' (0x5A) is not in our font and there's no U+FFFD, so getGlyph returns null.
  // "oZo" should lay out as: o1 at 0, Z skipped (advance flushed), o2 at 9.
  //   toPixel(145) = 9 (o's advance, no kern since Z resets prevCp).
  //   w = 9 + 8 = 17
  EXPECT_EQ(textWidth("oZo"), 17);

  // Multi-null: "oZZo" -- two consecutive nulls, advance still preserved.
  EXPECT_EQ(textWidth("oZZo"), 17);

  // Null at start: "Zo" -- no pending advance to flush, o renders at 0.
  EXPECT_EQ(textWidth("Zo"), 8);
}

TEST(EpdFont, HeightCalculation) {
  // 'T' is tallest: top=12, height=12 -> extent [0, 12)
  // 'o' and 'a': top=8, height=8 -> extent [0, 8)
  EXPECT_EQ(textHeight("o"), 8);
  EXPECT_EQ(textHeight("T"), 12);
  EXPECT_EQ(textHeight("To"), 12);
  EXPECT_EQ(textHeight("oo"), 8);
}

// ============================================================================
// Part 3: small-caps fold + scaled-width measurement
// ============================================================================

TEST(SmallCapsFold, AsciiLowercaseFolds) {
  uint32_t cp = 'a';
  EXPECT_TRUE(smallCaps::fold(cp));
  EXPECT_EQ(cp, static_cast<uint32_t>('A'));

  cp = 'z';
  EXPECT_TRUE(smallCaps::fold(cp));
  EXPECT_EQ(cp, static_cast<uint32_t>('Z'));
}

TEST(SmallCapsFold, NonLowercaseUnchanged) {
  for (uint32_t cp : {static_cast<uint32_t>('A'), static_cast<uint32_t>('Z'), static_cast<uint32_t>('5'),
                      static_cast<uint32_t>('.'), static_cast<uint32_t>(' ')}) {
    uint32_t v = cp;
    EXPECT_FALSE(smallCaps::fold(v));
    EXPECT_EQ(v, cp);
  }
}

TEST(SmallCapsFold, Latin1LowercaseFolds) {
  uint32_t cp = 0x00E9;  // é
  EXPECT_TRUE(smallCaps::fold(cp));
  EXPECT_EQ(cp, 0x00C9u);  // É

  cp = 0x00FC;  // ü
  EXPECT_TRUE(smallCaps::fold(cp));
  EXPECT_EQ(cp, 0x00DCu);  // Ü
}

TEST(SmallCapsFold, Latin1NonLetterExclusions) {
  uint32_t cp = 0x00F7;  // ÷ division sign — not a letter
  EXPECT_FALSE(smallCaps::fold(cp));
  EXPECT_EQ(cp, 0x00F7u);

  cp = 0x00DF;  // ß — no single-codepoint uppercase
  EXPECT_FALSE(smallCaps::fold(cp));
  EXPECT_EQ(cp, 0x00DFu);
}

TEST(SmallCapsFold, LatinExtendedAPairs) {
  uint32_t cp = 0x0101;  // ā
  EXPECT_TRUE(smallCaps::fold(cp));
  EXPECT_EQ(cp, 0x0100u);  // Ā

  cp = 0x0131;  // ı dotless i — intentionally not folded
  EXPECT_FALSE(smallCaps::fold(cp));
}

// Uppercase letters render full-size even under small-caps, so width is unchanged.
TEST(SmallCaps, UppercaseWidthUnchanged) { EXPECT_EQ(textWidthSC("T"), textWidth("T")); }

// 't' folds to the 'T' glyph (width 8) drawn at SCALE=0.75 -> round(8*0.75)=6.
// Single glyph at x=0 with left=0, so width == scaled glyph width.
TEST(SmallCaps, FoldedGlyphScaledWidth) {
  const int expected = static_cast<int>(8 * smallCaps::SCALE + 0.5f);  // 6
  EXPECT_EQ(textWidthSC("t"), expected);
  // Sanity: the folded small-cap is narrower than the full-size capital.
  EXPECT_LT(textWidthSC("t"), textWidth("T"));
}

// A folded run advances by the scaled advance between glyphs.
// "tt": glyph 'T' advance=137 FP. No kern (T has no right class, T has no left class
// in the matrix for T->T -> 0). Folded step = toPixel(round(137*0.75)) = toPixel(103) = 6.
// t1 scaled-width 6 at x=0; t2 at x=6, scaled width 6 -> w = 6 + 6 = 12.
TEST(SmallCaps, FoldedRunAdvance) {
  const int32_t scaledAdvFP = static_cast<int32_t>(137 * smallCaps::SCALE + 0.5f);  // 103
  const int step = fp4::toPixel(scaledAdvFP);                                       // 6
  const int scaledWidth = static_cast<int>(8 * smallCaps::SCALE + 0.5f);            // 6
  EXPECT_EQ(textWidthSC("tt"), step + scaledWidth);
}

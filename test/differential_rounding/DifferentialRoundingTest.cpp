#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "lib/EpdFont/EpdFont.h"
#include "lib/EpdFont/EpdFontData.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                                            \
  do {                                                                                             \
    if ((a) != (b)) {                                                                              \
      fprintf(stderr, "  FAIL: %s:%d: %s == %d, expected %d\n", __FILE__, __LINE__, #a, (a), (b)); \
      testsFailed++;                                                                               \
      return;                                                                                      \
    }                                                                                              \
  } while (0)

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                     \
      return;                                                            \
    }                                                                    \
  } while (0)

#define PASS() testsPassed++

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

// clang-format off
static const EpdGlyph kGlyphs[] = {
  // idx  width  height  advanceX  left  top  dataLength  dataOffset
  /* 0 'T' */ { 8, 12, 137, 0, 12, 0, 0 },
  /* 1 'a' */ { 7,  8, 130, 0,  8, 0, 0 },
  /* 2 'o' */ { 8,  8, 145, 0,  8, 0, 0 },
  /* 3 'x' */ { 7,  8, 136, 0,  8, 0, 0 },
};

static const EpdUnicodeInterval kIntervals[] = {
  { 0x54, 0x54, 0 },  // 'T' -> glyph[0]
  { 0x61, 0x61, 1 },  // 'a' -> glyph[1]
  { 0x6F, 0x6F, 2 },  // 'o' -> glyph[2]
  { 0x78, 0x78, 3 },  // 'x' -> glyph[3]
};

static const EpdKernClassEntry kKernLeft[] = {
  { 0x54, 1 },  // 'T' -> left class 1
  { 0x6F, 2 },  // 'o' -> left class 2
};

static const EpdKernClassEntry kKernRight[] = {
  { 0x61, 1 },  // 'a' -> right class 1
  { 0x6F, 2 },  // 'o' -> right class 2
};

// Flat matrix: leftClassCount(2) x rightClassCount(2), 4.4 fixed-point
//   [L1,R1]=kern(T,a)  [L1,R2]=kern(T,o)  [L2,R1]=kern(o,a)  [L2,R2]=kern(o,o)
static const int8_t kKernMatrix[] = { -5, -7, -2, -3 };

static const EpdFontData kTestFontData = {
  .bitmap            = nullptr,
  .glyph             = kGlyphs,
  .intervals         = kIntervals,
  .intervalCount     = 4,
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
};
// clang-format on

static EpdFont testFont(&kTestFontData);

// Helper: return width from getTextDimensions
static int textWidth(const char* str) {
  int w = 0, h = 0;
  testFont.getTextDimensions(str, &w, &h);
  return w;
}

static int textHeight(const char* str) {
  int w = 0, h = 0;
  testFont.getTextDimensions(str, &w, &h);
  return h;
}

// ============================================================================
// Part 1: Pure fp4 math tests
// ============================================================================

// Simulate the old absolute-snap gap for comparison
static int absoluteGap(int32_t startFP, int32_t advanceFP, int32_t kernFP) {
  int32_t nextFP = startFP + advanceFP + kernFP;
  return fp4::toPixel(nextFP) - fp4::toPixel(startFP);
}

void testFp4Basics() {
  printf("testFp4Basics...\n");

  for (int px = 0; px < 500; px++) {
    ASSERT_EQ(fp4::toPixel(fp4::fromPixel(px)), px);
  }

  ASSERT_EQ(fp4::toPixel(0), 0);
  ASSERT_EQ(fp4::toPixel(7), 0);    // 0.4375 -> 0
  ASSERT_EQ(fp4::toPixel(8), 1);    // 0.5 -> 1 (round half up)
  ASSERT_EQ(fp4::toPixel(15), 1);   // 0.9375 -> 1
  ASSERT_EQ(fp4::toPixel(16), 1);   // 1.0 -> 1
  ASSERT_EQ(fp4::toPixel(24), 2);   // 1.5 -> 2
  ASSERT_EQ(fp4::toPixel(-8), 0);   // -0.5 -> 0
  ASSERT_EQ(fp4::toPixel(-9), -1);  // -0.5625 -> -1
  ASSERT_EQ(fp4::toPixel(-16), -1);

  ASSERT_EQ(fp4::toPixel(137 + (-9)), 8);  // 128 = 8.0 exact
  ASSERT_EQ(fp4::toPixel(137 + (-5)), 8);  // 132 = 8.25
  ASSERT_EQ(fp4::toPixel(137 + (-1)), 9);  // 136 = 8.5 (half rounds up)

  printf("  All fp4 basics passed\n");
  PASS();
}

void testOldApproachInconsistency() {
  printf("testOldApproachInconsistency...\n");

  // 'oo' pair: advance=145 (9.0625px), kern=-3 (-0.1875px), combined=142 (8.875px)
  const int32_t advance = 145;
  const int32_t kern = -3;

  int minGap = 999, maxGap = -999;
  for (int startPx = 0; startPx < 100; startPx++) {
    for (int frac = 0; frac < 16; frac++) {
      int32_t startFP = fp4::fromPixel(startPx) + frac;
      int gap = absoluteGap(startFP, advance, kern);
      if (gap < minGap) minGap = gap;
      if (gap > maxGap) maxGap = gap;
    }
  }

  ASSERT_TRUE(maxGap - minGap >= 1);
  printf("  Old absolute gap range: [%d, %d] -- varies by %d px\n", minGap, maxGap, maxGap - minGap);

  int diffStep = fp4::toPixel(advance + kern);
  printf("  Differential step: always %d px\n", diffStep);
  PASS();
}

void testExhaustiveKernRange() {
  printf("testExhaustiveKernRange...\n");

  const int32_t baseAdvance = 128;
  int checked = 0;

  for (int advFrac = 0; advFrac < 16; advFrac++) {
    int32_t advance = baseAdvance + advFrac;
    for (int kern = -128; kern <= 127; kern++) {
      int step = fp4::toPixel(advance + static_cast<int32_t>(kern));
      float idealPx = fp4::toFloat(advance + kern);
      if (std::abs(step - idealPx) >= 1.0f) {
        fprintf(stderr, "  FAIL: advance=%d, kern=%d, step=%d, ideal=%.4f\n", advance, kern, step, idealPx);
        testsFailed++;
        return;
      }
      checked++;
    }
  }

  printf("  Checked %d (advance, kern) combinations -- all within 1px of ideal\n", checked);
  PASS();
}

// ============================================================================
// Part 2: Integration tests using real EpdFont::getTextDimensions
// ============================================================================

void testKernLookup() {
  printf("testKernLookup...\n");

  ASSERT_EQ(testFont.getKerning('T', 'a'), -5);
  ASSERT_EQ(testFont.getKerning('T', 'o'), -7);
  ASSERT_EQ(testFont.getKerning('o', 'a'), -2);
  ASSERT_EQ(testFont.getKerning('o', 'o'), -3);
  ASSERT_EQ(testFont.getKerning('a', 'o'), 0);  // 'a' has no left class
  ASSERT_EQ(testFont.getKerning('x', 'o'), 0);  // 'x' has no left class
  ASSERT_EQ(testFont.getKerning('T', 'x'), 0);  // 'x' has no right class
  ASSERT_EQ(testFont.getKerning('T', 'T'), 0);  // 'T' has no right class

  printf("  All kern lookups correct\n");
  PASS();
}

void testGlyphLookup() {
  printf("testGlyphLookup...\n");

  ASSERT_TRUE(testFont.getGlyph('T') != nullptr);
  ASSERT_TRUE(testFont.getGlyph('a') != nullptr);
  ASSERT_TRUE(testFont.getGlyph('o') != nullptr);
  ASSERT_TRUE(testFont.getGlyph('x') != nullptr);
  ASSERT_EQ(testFont.getGlyph('T')->advanceX, 137);
  ASSERT_EQ(testFont.getGlyph('a')->advanceX, 130);
  ASSERT_EQ(testFont.getGlyph('o')->advanceX, 145);
  ASSERT_EQ(testFont.getGlyph('x')->advanceX, 136);

  // No U+FFFD in font, so unknown codepoints return nullptr
  ASSERT_TRUE(testFont.getGlyph('Z') == nullptr);
  ASSERT_TRUE(testFont.getGlyph('b') == nullptr);

  printf("  All glyph lookups correct\n");
  PASS();
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
void testKnownWidths() {
  printf("testKnownWidths...\n");

  // "o": single glyph at x=0, width=8
  //   w = 0 + 8 = 8
  ASSERT_EQ(textWidth("o"), 8);

  // "oo": step = toPixel(145 + (-3)) = toPixel(142) = 9
  //   o1 at 0, o2 at 9.  w = 9 + 8 = 17
  ASSERT_EQ(textWidth("oo"), 17);

  // "ooo": two steps of 9
  //   o1 at 0, o2 at 9, o3 at 18.  w = 18 + 8 = 26
  ASSERT_EQ(textWidth("ooo"), 26);

  // "To": step = toPixel(137 + (-7)) = toPixel(130) = 8
  //   T at 0, o at 8.  w = 8 + 8 = 16
  ASSERT_EQ(textWidth("To"), 16);

  // "Ta": step = toPixel(137 + (-5)) = toPixel(132) = 8
  //   T at 0, a at 8.  w = 8 + 7 = 15
  ASSERT_EQ(textWidth("Ta"), 15);

  // "oa": step = toPixel(145 + (-2)) = toPixel(143) = 9
  //   o at 0, a at 9.  w = 9 + 7 = 16
  ASSERT_EQ(textWidth("oa"), 16);

  // "Too": T at 0.
  //   step T->o = toPixel(137 + (-7)) = 8.  o1 at 8.
  //   step o->o = toPixel(145 + (-3)) = 9.  o2 at 17.
  //   w = 17 + 8 = 25
  ASSERT_EQ(textWidth("Too"), 25);

  // "xo": step = toPixel(136 + 0) = toPixel(136) = 9  (no kern: x has no left class)
  //   x at 0, o at 9.  w = 9 + 8 = 17
  ASSERT_EQ(textWidth("xo"), 17);

  printf("  All known widths correct\n");
  PASS();
}

// "oo" pair consistency: the pixel gap between two o's must be the same
// regardless of what prefix precedes them.  This is THE key property of
// differential rounding.  With absolute snapping, "xoo" would produce a
// different oo gap than "oo" because 'x' advance (136 FP) puts the first
// 'o' at fractional phase 8, crossing the rounding boundary differently.
void testPairConsistencyViaFont() {
  printf("testPairConsistencyViaFont...\n");

  // The oo gap = width(prefix + "oo") - width(prefix + "o")
  // This isolates the pixel distance contributed by the second 'o'.
  const int oo_gap_bare = textWidth("oo") - textWidth("o");
  const int oo_gap_after_x = textWidth("xoo") - textWidth("xo");
  const int oo_gap_after_T = textWidth("Too") - textWidth("To");
  const int oo_gap_after_o = textWidth("ooo") - textWidth("oo");

  printf("  oo gap (bare):    %d\n", oo_gap_bare);
  printf("  oo gap (after x): %d\n", oo_gap_after_x);
  printf("  oo gap (after T): %d\n", oo_gap_after_T);
  printf("  oo gap (after o): %d\n", oo_gap_after_o);

  // All must be identical
  ASSERT_EQ(oo_gap_after_x, oo_gap_bare);
  ASSERT_EQ(oo_gap_after_T, oo_gap_bare);
  ASSERT_EQ(oo_gap_after_o, oo_gap_bare);

  printf("  All oo gaps identical (%d px) regardless of prefix\n", oo_gap_bare);
  PASS();
}

// Null-glyph handling: when a codepoint has no glyph (and no replacement
// glyph), the pending advance from the previous glyph must still be flushed.
// Without the flush fix, the glyph after the null would overlap the one before.
void testNullGlyphAdvancePreserved() {
  printf("testNullGlyphAdvancePreserved...\n");

  // 'Z' (0x5A) is not in our font and there's no U+FFFD, so getGlyph returns null.
  // "oZo" should lay out as: o1 at 0, Z skipped (advance flushed), o2 at 9.
  //   toPixel(145) = 9 (o's advance, no kern since Z resets prevCp).
  //   w = 9 + 8 = 17
  int w = textWidth("oZo");
  printf("  width(\"oZo\") = %d\n", w);

  // Without the flush fix, o2 would land at 0 (overlapping o1), giving w = 8.
  ASSERT_TRUE(w > 8);
  ASSERT_EQ(w, 17);

  // Multi-null: "oZZo" -- two consecutive nulls, advance still preserved.
  w = textWidth("oZZo");
  printf("  width(\"oZZo\") = %d\n", w);
  ASSERT_EQ(w, 17);

  // Null at start: "Zo" -- no pending advance to flush, o renders at 0.
  w = textWidth("Zo");
  printf("  width(\"Zo\") = %d\n", w);
  ASSERT_EQ(w, 8);

  printf("  Null-glyph advance correctly preserved\n");
  PASS();
}

void testHeightCalculation() {
  printf("testHeightCalculation...\n");

  // 'T' is tallest: top=12, height=12 -> extent [0, 12)
  // 'o' and 'a': top=8, height=8 -> extent [0, 8)
  ASSERT_EQ(textHeight("o"), 8);
  ASSERT_EQ(textHeight("T"), 12);
  ASSERT_EQ(textHeight("To"), 12);
  ASSERT_EQ(textHeight("oo"), 8);

  printf("  All heights correct\n");
  PASS();
}

int main() {
  printf("=== Differential Rounding Tests ===\n\n");

  // Part 1: Pure fp4 math
  testFp4Basics();
  testOldApproachInconsistency();
  testExhaustiveKernRange();

  // Part 2: Integration tests against real EpdFont
  testKernLookup();
  testGlyphLookup();
  testKnownWidths();
  testPairConsistencyViaFont();
  testNullGlyphAdvancePreserved();
  testHeightCalculation();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}

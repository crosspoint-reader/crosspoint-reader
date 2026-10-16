// Host tests for the last-resort glyph substitution.

#include <gtest/gtest.h>

#include "GlyphFallback.h"

namespace {

TEST(GlyphFallback, LeavesCoveredCodepointsAlone) {
  // Everything a font is expected to carry must pass through untouched --
  // the substitution is only ever consulted after a real lookup failed, and
  // returning something else here would mean silently redrawing normal text.
  for (uint32_t cp = 0; cp < 0x0250; cp++) {
    EXPECT_EQ(fallbackGlyphCodepoint(cp), cp) << "U+" << std::hex << cp;
  }
  EXPECT_EQ(fallbackGlyphCodepoint(0x0300), 0x0300u);    // combining marks
  EXPECT_EQ(fallbackGlyphCodepoint(0x0416), 0x0416u);    // Cyrillic
  EXPECT_EQ(fallbackGlyphCodepoint(0x4E00), 0x4E00u);    // CJK
  EXPECT_EQ(fallbackGlyphCodepoint(0x1F600), 0x1F600u);  // above the BMP
}

TEST(GlyphFallback, MapsPhoneticLettersToTheirLatinShape) {
  EXPECT_EQ(fallbackGlyphCodepoint(0x0259), static_cast<uint32_t>('e'));  // schwa
  EXPECT_EQ(fallbackGlyphCodepoint(0x026A), static_cast<uint32_t>('I'));  // small capital I
  EXPECT_EQ(fallbackGlyphCodepoint(0x028A), static_cast<uint32_t>('U'));  // upsilon
  EXPECT_EQ(fallbackGlyphCodepoint(0x0251), static_cast<uint32_t>('a'));  // script a
  EXPECT_EQ(fallbackGlyphCodepoint(0x0254), static_cast<uint32_t>('o'));  // open o
  EXPECT_EQ(fallbackGlyphCodepoint(0x0292), static_cast<uint32_t>('z'));  // ezh
  EXPECT_EQ(fallbackGlyphCodepoint(0x028C), static_cast<uint32_t>('v'));  // turned v
  EXPECT_EQ(fallbackGlyphCodepoint(0x0261), static_cast<uint32_t>('g'));  // script g really is a g
}

TEST(GlyphFallback, MapsStressAndLengthMarksToPunctuation) {
  EXPECT_EQ(fallbackGlyphCodepoint(0x02C8), static_cast<uint32_t>('\''));  // primary stress
  EXPECT_EQ(fallbackGlyphCodepoint(0x02CC), static_cast<uint32_t>(','));   // secondary stress
  EXPECT_EQ(fallbackGlyphCodepoint(0x02D0), static_cast<uint32_t>(':'));   // length mark
}

TEST(GlyphFallback, MapsBoxDrawingToAscii) {
  // No built-in font carries U+2500-U+257F. The MD reader's table rows use the
  // light vertical as a cell divider (#471); tree listings in code blocks use
  // the rest of the light set.
  EXPECT_EQ(fallbackGlyphCodepoint(0x2502), static_cast<uint32_t>('|'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x2500), static_cast<uint32_t>('-'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x250C), static_cast<uint32_t>('+'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x2514), static_cast<uint32_t>('+'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x251C), static_cast<uint32_t>('+'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x253C), static_cast<uint32_t>('+'));
  // Heavy and double variants are NOT mapped: they are rare outside terminal
  // art, and an unmapped codepoint still draws the replacement glyph.
  EXPECT_EQ(fallbackGlyphCodepoint(0x2503), 0x2503u);
  EXPECT_EQ(fallbackGlyphCodepoint(0x2550), 0x2550u);
}

TEST(GlyphFallback, MapsBulletShapesToABullet) {
  // U+2022 is in the General Punctuation range every font here carries.
  EXPECT_EQ(fallbackGlyphCodepoint(0x25AA), 0x2022u);
  EXPECT_EQ(fallbackGlyphCodepoint(0x25C6), 0x2022u);
}

TEST(GlyphFallback, MapsTheSymbolsDictionariesUseAsMarkers) {
  // Noto Sans -- the font definitions are pinned to -- ships none of the arrow
  // or math block, and dictionaries use it structurally. The tilde operator
  // alone stands in for the headword 107k times in PONS En-De.
  EXPECT_EQ(fallbackGlyphCodepoint(0x223C), static_cast<uint32_t>('~'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x2248), static_cast<uint32_t>('~'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x2191), static_cast<uint32_t>('^'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x2192), static_cast<uint32_t>('>'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x222B), static_cast<uint32_t>('f'));
  // Latin Extended Additional: only its Vietnamese subrange is shipped, so
  // these arrive here and fall back to the base letter.
  EXPECT_EQ(fallbackGlyphCodepoint(0x1E37), static_cast<uint32_t>('l'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x1E71), static_cast<uint32_t>('t'));
  EXPECT_EQ(fallbackGlyphCodepoint(0x039B), static_cast<uint32_t>('A'));
}

TEST(GlyphFallback, HasNoMappingForCodepointsWithNoGoodStandIn) {
  // Greek letters IPA borrows have no Latin lookalike; a wrong guess would be
  // worse than the box, so they are deliberately absent from the table.
  EXPECT_EQ(fallbackGlyphCodepoint(0x03B8), 0x03B8u);  // theta -- shipped in the dictionary font
  EXPECT_EQ(fallbackGlyphCodepoint(0x00F0), 0x00F0u);  // eth -- Latin-1, always covered anyway
}

TEST(GlyphFallback, SubstitutesAreThemselvesNeverSubstituted) {
  // A substitution must terminate: whatever the table maps to has to be a
  // codepoint the table leaves alone, or a caller retrying could loop.
  for (uint32_t cp = 0x0250; cp <= 0x25FF; cp++) {
    const uint32_t once = fallbackGlyphCodepoint(cp);
    if (once == cp) continue;
    EXPECT_EQ(fallbackGlyphCodepoint(once), once) << "U+" << std::hex << cp << " -> U+" << once << " -> further";
  }
}

}  // namespace

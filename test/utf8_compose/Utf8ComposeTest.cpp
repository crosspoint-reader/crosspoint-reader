#include <gtest/gtest.h>

#include <string>

#include "Utf8.h"

namespace {

// Helpers to build NFD / expected byte sequences explicitly so the test does not
// depend on the encoding of this source file.
const std::string kCombGrave = "\xCC\x80";     // U+0300 COMBINING GRAVE ACCENT
const std::string kCombAcute = "\xCC\x81";     // U+0301 COMBINING ACUTE ACCENT
const std::string kCombCirc = "\xCC\x82";      // U+0302 COMBINING CIRCUMFLEX ACCENT
const std::string kCombDotBelow = "\xCC\xA3";  // U+0323 COMBINING DOT BELOW

}  // namespace

// ASCII and already-precomposed (NFC) text must pass through untouched (fast path).
TEST(Utf8ComposeNfc, PassesThroughAsciiAndNfc) {
  EXPECT_EQ(utf8ComposeNfc(""), "");
  EXPECT_EQ(utf8ComposeNfc("hello world"), "hello world");
  EXPECT_EQ(utf8ComposeNfc("caf\xC3\xA9"), "caf\xC3\xA9");  // é already U+00E9
}

// Single combining mark composes onto its base letter.
TEST(Utf8ComposeNfc, ComposesSingleMark) {
  EXPECT_EQ(utf8ComposeNfc("e" + kCombAcute), "\xC3\xA9");  // e + ́  -> é  (U+00E9)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombGrave), "\xC3\xA0");  // a + ̀  -> à  (U+00E0)
}

// Vietnamese letters carry two stacked marks; composition must accumulate them
// onto the intermediate precomposed character (this is the crux of the feature).
TEST(Utf8ComposeNfc, ComposesStackedVietnameseMarks) {
  // a + circumflex + acute -> ấ (U+1EA5)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombCirc + kCombAcute), "\xE1\xBA\xA5");
  // a + dot-below + circumflex (canonical order) -> ậ (U+1EAD)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombDotBelow + kCombCirc), "\xE1\xBA\xAD");
}

// A combining mark with no composition for its base is left unchanged, and the
// base is preserved.
TEST(Utf8ComposeNfc, LeavesUncomposableMarksIntact) {
  const std::string in = "q" + kCombAcute;  // no precomposed "q with acute"
  EXPECT_EQ(utf8ComposeNfc(in), in);
}

// A leading combining mark (no preceding base) is emitted unchanged.
TEST(Utf8ComposeNfc, HandlesLeadingMark) { EXPECT_EQ(utf8ComposeNfc(kCombAcute), kCombAcute); }

// Marks embedded in a longer word compose while surrounding text is preserved.
TEST(Utf8ComposeNfc, ComposesWithinWord) {
  // "Ti" + e+circ+acute + "ng" -> "Tiếng"
  EXPECT_EQ(utf8ComposeNfc("Ti" + std::string("e") + kCombCirc + kCombAcute + "ng"), "Ti\xE1\xBA\xBFng");
}

// Thai above/below vowels and tone marks are zero-advance overlays; spacing
// Thai characters (consonants, leading/trailing vowels, sara am) are not.
TEST(Utf8IsCombiningMark, ClassifiesThaiMarks) {
  EXPECT_TRUE(utf8IsCombiningMark(0x0E31));   // mai han-akat
  EXPECT_TRUE(utf8IsCombiningMark(0x0E34));   // sara i
  EXPECT_TRUE(utf8IsCombiningMark(0x0E37));   // sara uee
  EXPECT_TRUE(utf8IsCombiningMark(0x0E38));   // sara u (below)
  EXPECT_TRUE(utf8IsCombiningMark(0x0E3A));   // phinthu
  EXPECT_TRUE(utf8IsCombiningMark(0x0E48));   // mai ek
  EXPECT_TRUE(utf8IsCombiningMark(0x0E4B));   // mai chattawa
  EXPECT_TRUE(utf8IsCombiningMark(0x0E4C));   // thanthakhat
  EXPECT_FALSE(utf8IsCombiningMark(0x0E01));  // ko kai (consonant)
  EXPECT_FALSE(utf8IsCombiningMark(0x0E32));  // sara aa (spacing)
  EXPECT_FALSE(utf8IsCombiningMark(0x0E33));  // sara am (spacing)
  EXPECT_FALSE(utf8IsCombiningMark(0x0E40));  // sara e (spacing, leading)
  EXPECT_FALSE(utf8IsCombiningMark(0x0E46));  // maiyamok (spacing)
}

TEST(Utf8IsThaiCodepoint, CoversThaiBlockOnly) {
  EXPECT_TRUE(utf8IsThaiCodepoint(0x0E01));   // ko kai
  EXPECT_TRUE(utf8IsThaiCodepoint(0x0E48));   // mai ek
  EXPECT_TRUE(utf8IsThaiCodepoint(0x0E5B));   // khomut
  EXPECT_FALSE(utf8IsThaiCodepoint(0x0041));  // Latin A
  EXPECT_FALSE(utf8IsThaiCodepoint(0x4E00));  // CJK ideograph
  EXPECT_FALSE(utf8IsThaiCodepoint(0x0E80));  // Lao block starts here
}

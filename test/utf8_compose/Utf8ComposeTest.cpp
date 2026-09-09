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

// --- Hangul LV / LVT composition (macOS writes filenames in NFD) ---

namespace {
const std::string kJamoG = "\xE1\x84\x80";   // U+1100 CHOSEONG KIYEOK (leading ㄱ)
const std::string kJamoH = "\xE1\x84\x92";   // U+1112 CHOSEONG HIEUH (leading ㅎ)
const std::string kJamoA = "\xE1\x85\xA1";   // U+1161 JUNGSEONG A (medial ㅏ)
const std::string kJamoN = "\xE1\x86\xAB";   // U+11AB JONGSEONG NIEUN (trailing ㄴ)
const std::string kJamoNg = "\xE1\x86\xBC";  // U+11BC JONGSEONG IEUNG (trailing ㅇ)
}  // namespace

// A leading consonant + medial vowel composes to an LV syllable (U+AC00 block).
TEST(Utf8ComposeNfc, ComposesHangulLv) {
  EXPECT_EQ(utf8ComposeNfc(kJamoH + kJamoA), "\xED\x95\x98");  // ㅎ(U+1112) + ㅏ(U+1161) -> 하 (U+D558)
}

// LV + trailing consonant extends to an LVT syllable.
TEST(Utf8ComposeNfc, ComposesHangulLvt) {
  // ㅎ(U+1112) + ㅏ(U+1161) + ㄴ(U+11AB) -> 한 (U+D55C)
  EXPECT_EQ(utf8ComposeNfc(kJamoH + kJamoA + kJamoN), "\xED\x95\x9C");
}

// Consecutive LVT syllables compose independently within a word.
TEST(Utf8ComposeNfc, ComposesHangulWord) {
  // 한 + 강 in NFD -> 한강 (U+D55C U+AC15)
  const std::string nfd = kJamoH + kJamoA + kJamoN + kJamoG + kJamoA + kJamoNg;
  EXPECT_EQ(utf8ComposeNfc(nfd), "\xED\x95\x9C\xEA\xB0\x95");
}

// A real NFD filename: "해커와 화가.epub" (Hackers & Painters) exactly as it
// appears on a FAT SD card after a Finder copy from macOS -- decomposed
// conjoining jamo. A run of LV syllables composes while the space and ASCII
// extension are preserved. Bytes verified against Python unicodedata output.
TEST(Utf8ComposeNfc, ComposesRealNfdFilename) {
  const std::string nfd =
      "\xE1\x84\x92\xE1\x85\xA2\xE1\x84\x8F\xE1\x85\xA5\xE1\x84\x8B\xE1\x85\xAA"  // 해커와 (NFD)
      " "
      "\xE1\x84\x92\xE1\x85\xAA\xE1\x84\x80\xE1\x85\xA1"  // 화가 (NFD)
      ".epub";
  EXPECT_EQ(utf8ComposeNfc(nfd), "\xED\x95\xB4\xEC\xBB\xA4\xEC\x99\x80 \xED\x99\x94\xEA\xB0\x80.epub");
}

// Jamo that cannot start a syllable pass through unchanged: a lone leading
// consonant, a lone medial/trailing, and a leading consonant not followed by a
// medial vowel.
TEST(Utf8ComposeNfc, LeavesUncomposableJamoIntact) {
  EXPECT_EQ(utf8ComposeNfc(kJamoH), kJamoH);
  EXPECT_EQ(utf8ComposeNfc(kJamoA + kJamoN), kJamoA + kJamoN);
  EXPECT_EQ(utf8ComposeNfc(kJamoH + "a"), kJamoH + "a");
}

// Precomposed (NFC) Hangul takes the fast path untouched: syllables encode
// with lead bytes EA-ED, not the 0xE1 the scan looks for.
TEST(Utf8ComposeNfc, PassesThroughPrecomposedHangul) {
  EXPECT_EQ(utf8ComposeNfc("\xED\x95\x9C\xEA\xB0\x95"), "\xED\x95\x9C\xEA\xB0\x95");  // 한강
}

// Other scripts sharing the 0xE1 lead byte (U+1000-1FFF) fall through the full
// pass but come out byte-identical.
TEST(Utf8ComposeNfc, LeavesOtherE1ScriptsIntact) {
  const std::string georgian = "\xE1\x83\xA5\xE1\x83\x90";  // ქა (U+10E5 U+10D0)
  EXPECT_EQ(utf8ComposeNfc(georgian), georgian);
  const std::string cherokee = "\xE1\x8F\xA3";  // Ꮳ (U+13E3)
  EXPECT_EQ(utf8ComposeNfc(cherokee), cherokee);
}

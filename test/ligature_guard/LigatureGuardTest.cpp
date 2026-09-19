// LigatureGuardTest — font GSUB ligatures must not re-process Arabic text.
//
// Arabic contextual joining (including Lam-Alef) is resolved at render time by
// do_shape() in MiniBidi, which emits Arabic presentation forms in visual
// order. A font's GSUB ligature table (extracted from the source font) also
// carries Lam-Alef pairs keyed on those presentation forms, e.g.
//   FEDF (lam-initial) + FE8E (alef-final) -> FEFB (lam-alef isolated)
// If EpdFont::getLigature() ran that pair over already-shaped text, a normal
// Alef+Lam ("…ال…", shaped to FEDF FE8E) would be wrongly collapsed into a
// Lam-Alef ligature — transposing the letters (e.g. کسالت -> کسلات).
//
// getLigature() therefore refuses any pair whose operands are Arabic
// presentation forms. Latin ligatures (ff/fi/fl, keyed on ASCII) are kept.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "Utf8.h"

namespace {

// Sorted-by-`pair` ligature table (getLigature binary-searches it): a Latin
// ff, a base-codepoint Arabic composition, and the shaped Lam-Alef pair.
const EpdLigaturePair kPairs[] = {
    {(0x0066u << 16) | 0x0066u, 0xFB00u},  // f + f              -> ff (Latin)
    {(0x0627u << 16) | 0x0653u, 0x0622u},  // alef + maddah above -> آ (base Arabic)
    {(0xFEDFu << 16) | 0xFE8Eu, 0xFEFBu},  // lam-init + alef-fin -> lam-alef (form)
};

EpdFont makeFont(EpdFontData& data) {
  std::memset(&data, 0, sizeof(data));
  data.ligaturePairs = kPairs;
  data.ligaturePairCount = sizeof(kPairs) / sizeof(kPairs[0]);
  return EpdFont(&data);
}

}  // namespace

// Latin ligatures operate on ASCII and must keep working.
TEST(LigatureGuard, LatinLigatureKept) {
  EpdFontData data;
  const EpdFont font = makeFont(data);
  EXPECT_EQ(font.getLigature(0x0066, 0x0066), 0xFB00u);
}

// The Arabic Lam-Alef GSUB pair must be suppressed: do_shape() already owns it.
TEST(LigatureGuard, ArabicPresentationFormPairSuppressed) {
  EpdFontData data;
  const EpdFont font = makeFont(data);
  EXPECT_EQ(font.getLigature(0xFEDF, 0xFE8E), 0u);
}

// Base-codepoint Arabic ligatures (operands below the presentation-form
// ranges) are NOT shaper output and must still apply — the guard is not
// over-broad. Alef + combining maddah composes to precomposed آ (U+0622).
TEST(LigatureGuard, BaseArabicCompositionKept) {
  EpdFontData data;
  const EpdFont font = makeFont(data);
  EXPECT_EQ(font.getLigature(0x0627, 0x0653), 0x0622u);
}

// End-to-end: a shaped Alef+Lam fragment (FEDF followed by FE8E) must not
// collapse — the two letters stay distinct, so کسالت keeps its ال.
TEST(LigatureGuard, ShapedAlefLamNotCollapsed) {
  EpdFontData data;
  const EpdFont font = makeFont(data);

  std::string tail;  // the codepoint that follows the FEDF start: FE8E
  utf8AppendCodepoint(0xFE8E, tail);
  const char* cursor = tail.c_str();

  EXPECT_EQ(font.applyLigatures(0xFEDF, cursor), 0xFEDFu);
}

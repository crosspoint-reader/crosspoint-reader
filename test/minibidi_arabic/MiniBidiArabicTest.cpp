// MiniBidiArabicTest — Arabic/Farsi/Urdu bidi + contextual shaping.
//
// Exercises the full BidiUtils::applyBidiVisual() pipeline (the single code
// path shared by GfxRenderer::getTextWidth() and drawText()): UAX#9
// reordering (do_bidi) followed by mintty-ported Arabic shaping (do_shape).
//
// Inputs are built from codepoint arrays in LOGICAL order; expectations are
// codepoint arrays in VISUAL order (left to right, as drawn on screen).
// Presentation-form expectations were derived by hand from Unicode
// ArabicShaping.txt joining types and the Arabic Presentation Forms-A/B
// blocks of UnicodeData.txt.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "BidiUtils.h"
#include "Utf8.h"

namespace {

std::string encode(const std::vector<uint32_t>& codepoints) {
  std::string utf8;
  for (const uint32_t cp : codepoints) utf8AppendCodepoint(cp, utf8);
  return utf8;
}

std::vector<uint32_t> decode(const std::string& utf8) {
  std::vector<uint32_t> codepoints;
  auto* p = reinterpret_cast<const unsigned char*>(utf8.c_str());
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp) break;
    codepoints.push_back(cp);
  }
  return codepoints;
}

// Runs the shared bidi+shape pipeline with auto-detected paragraph direction
// and returns the visual-order codepoints.
std::vector<uint32_t> shapeVisual(const std::vector<uint32_t>& logical) {
  const std::string in = encode(logical);
  std::string out;
  if (!BidiUtils::applyBidiVisual(in.c_str(), out, /*paragraphLevel=*/-1)) {
    return decode(in);  // pipeline declined: unchanged text
  }
  return decode(out);
}

using CP = std::vector<uint32_t>;

}  // namespace

/* ── Core Arabic contextual forms ────────────────────────────────────── */

// A single letter renders in its isolated presentation form.
TEST(ArabicShaping, SingleLetterIsolated) {
  EXPECT_EQ(shapeVisual({0x0628}), (CP{0xFE8F}));  // ب
}

// دار: dal and alef are right-joining, so no letter connects forward —
// all three stay isolated. Visual order is reversed.
TEST(ArabicShaping, RightJoinersStayIsolated) {
  EXPECT_EQ(shapeVisual({0x062F, 0x0627, 0x0631}), (CP{0xFEAD, 0xFE8D, 0xFEA9}));
}

// بيت: initial + medial + final chain of dual-joining letters.
TEST(ArabicShaping, DualJoiningChain) {
  EXPECT_EQ(shapeVisual({0x0628, 0x064A, 0x062A}), (CP{0xFE96, 0xFEF4, 0xFE91}));
}

// محمد: medial forms across four letters ending in right-joining dal.
TEST(ArabicShaping, MedialForms) {
  EXPECT_EQ(shapeVisual({0x0645, 0x062D, 0x0645, 0x062F}), (CP{0xFEAA, 0xFEE4, 0xFEA4, 0xFEE3}));
}

// Tatweel (U+0640) is join-causing and passes through unchanged.
TEST(ArabicShaping, TatweelJoinsBothSides) {
  EXPECT_EQ(shapeVisual({0x0628, 0x0640, 0x062A}), (CP{0xFE96, 0x0640, 0xFE91}));
}

/* ── Lam-Alef ligatures ──────────────────────────────────────────────── */

// All four Lam-Alef variants, isolated (nothing joins into the Lam).
// The absorbed Alef must be collapsed out of the output entirely.
TEST(ArabicShaping, LamAlefIsolatedVariants) {
  EXPECT_EQ(shapeVisual({0x0644, 0x0627}), (CP{0xFEFB}));  // لا
  EXPECT_EQ(shapeVisual({0x0644, 0x0622}), (CP{0xFEF5}));  // لآ
  EXPECT_EQ(shapeVisual({0x0644, 0x0623}), (CP{0xFEF7}));  // لأ
  EXPECT_EQ(shapeVisual({0x0644, 0x0625}), (CP{0xFEF9}));  // لإ
}

// All four Lam-Alef variants in final form (a joining letter precedes Lam).
TEST(ArabicShaping, LamAlefFinalVariants) {
  EXPECT_EQ(shapeVisual({0x0628, 0x0644, 0x0627}), (CP{0xFEFC, 0xFE91}));
  EXPECT_EQ(shapeVisual({0x0628, 0x0644, 0x0622}), (CP{0xFEF6, 0xFE91}));
  EXPECT_EQ(shapeVisual({0x0628, 0x0644, 0x0623}), (CP{0xFEF8, 0xFE91}));
  EXPECT_EQ(shapeVisual({0x0628, 0x0644, 0x0625}), (CP{0xFEFA, 0xFE91}));
}

// The specific bug flagged in PR #2398 review: a diacritic between Lam and
// Alef must not break the ligature. The fatha stays in the stream (a
// zero-advance overlay at render time); the Alef is absorbed. Per UAX#9 L3
// the mark is emitted after the ligature it decorates.
TEST(ArabicShaping, LamAlefWithDiacriticBetween) {
  EXPECT_EQ(shapeVisual({0x0644, 0x064E, 0x0627}), (CP{0xFEFB, 0x064E}));
}

/* ── Harakat (diacritics) ────────────────────────────────────────────── */

// كَتَبَ fully vocalized: harakat are transparent for joining — the letter
// skeleton shapes exactly as كتب — and remain in the output stream. UAX#9
// rule L3: each mark is emitted after its base, so the renderer can overlay
// it on the most recently drawn glyph.
TEST(ArabicShaping, VocalizedTextJoinsAcrossHarakat) {
  EXPECT_EQ(shapeVisual({0x0643, 0x064E, 0x062A, 0x064E, 0x0628, 0x064E}),
            (CP{0xFE90, 0x064E, 0xFE98, 0x064E, 0xFEDB, 0x064E}));
}

// Stacked marks (shadda + fatha on one base) keep their logical order after
// the base, so above-base stacking renders bottom-up as authored.
TEST(ArabicShaping, StackedMarksFollowBaseInLogicalOrder) {
  EXPECT_EQ(shapeVisual({0x0628, 0x0651, 0x064E}), (CP{0xFE8F, 0x0651, 0x064E}));
}

// Hebrew niqqud rides the same L3 path: mark follows its base after the
// RTL run is reversed.
TEST(HebrewShaping, NiqqudFollowsBase) {
  EXPECT_EQ(shapeVisual({0x05E9, 0x05B0, 0x05DC}), (CP{0x05DC, 0x05E9, 0x05B0}));
}

/* ── Farsi extra letters ─────────────────────────────────────────────── */

// Peh joins like beh, using Presentation Forms-A codepoints.
TEST(FarsiShaping, PehContextualForms) { EXPECT_EQ(shapeVisual({0x067E, 0x067E}), (CP{0xFB57, 0xFB58})); }

// گاز: gaf initial, alef final, jeh-group zain isolated.
TEST(FarsiShaping, GafWord) { EXPECT_EQ(shapeVisual({0x06AF, 0x0627, 0x0632}), (CP{0xFEAF, 0xFE8E, 0xFB94})); }

// سیب: Farsi yeh (U+06CC) takes its medial Presentation Forms-A form.
TEST(FarsiShaping, FarsiYehMedial) { EXPECT_EQ(shapeVisual({0x0633, 0x06CC, 0x0628}), (CP{0xFE90, 0xFBFF, 0xFEB3})); }

/* ── Urdu extra letters ──────────────────────────────────────────────── */

// ٹ (U+0679) and ڑ (U+0691) were specifically flagged in PR #2398 review as
// missing from a literal mintty port; ڈ (U+0688) completes the retroflex set.
TEST(UrduShaping, RetroflexLetters) { EXPECT_EQ(shapeVisual({0x0679, 0x0688, 0x0691}), (CP{0xFB8C, 0xFB89, 0xFB68})); }

// میں: noon ghunna (U+06BA) is dual-joining but only has isolated/final
// presentation forms — final position works.
TEST(UrduShaping, NoonGhunnaFinal) { EXPECT_EQ(shapeVisual({0x0645, 0x06CC, 0x06BA}), (CP{0xFB9F, 0xFBFF, 0xFEE3})); }

// ہے: heh goal initial + yeh barree final.
TEST(UrduShaping, HehGoalYehBarree) { EXPECT_EQ(shapeVisual({0x06C1, 0x06D2}), (CP{0xFBAF, 0xFBA8})); }

/* ── Sindhi / Pashto / Kurdish samples ───────────────────────────────── */

// Sindhi ٻار: beeh (U+067B) initial form from Presentation Forms-A.
TEST(SindhiShaping, BeehInitial) { EXPECT_EQ(shapeVisual({0x067B, 0x0627, 0x0631}), (CP{0xFEAD, 0xFE8E, 0xFB54})); }

// Pashto ښه: seen-with-dots (U+069A) has a joining type but NO presentation
// forms — it keeps its base codepoint while its neighbour still takes the
// correct joined form.
TEST(PashtoShaping, LetterWithoutPresentationFormsFallsBack) {
  EXPECT_EQ(shapeVisual({0x069A, 0x0647}), (CP{0xFEEA, 0x069A}));
}

// Pashto کور: keheh initial, waw final, reh isolated.
TEST(PashtoShaping, KehehWord) { EXPECT_EQ(shapeVisual({0x06A9, 0x0648, 0x0631}), (CP{0xFEAD, 0xFEEE, 0xFB90})); }

// Kurdish ڕۆژ: all right-joining; ڕ (U+0695) has no presentation forms and
// stays as its base codepoint, ۆ and ژ take Presentation Forms-A isolated.
TEST(KurdishShaping, RightJoiningLetters) {
  EXPECT_EQ(shapeVisual({0x0695, 0x06C6, 0x0698}), (CP{0xFB8A, 0xFBD9, 0x0695}));
}

/* ── ZWJ / ZWNJ joining formatters ───────────────────────────────────── */

// ZWNJ blocks joining across it but leaves the outer sides to normal
// contextual rules (Farsi morphology, e.g. می‌خواهم). The formatter itself
// must be filtered from the output. Here meem joins into yeh (initial +
// final) while the ZWNJ keeps yeh from connecting to khah.
TEST(JoinerShaping, ZwnjBreaksJoin) {
  // می‌خ: trailing khah has no forward partner → isolated.
  EXPECT_EQ(shapeVisual({0x0645, 0x06CC, 0x200C, 0x062E}), (CP{0xFEA5, 0xFBFD, 0xFEE3}));
  // می‌خو: waw follows khah → khah initial, waw final.
  EXPECT_EQ(shapeVisual({0x0645, 0x06CC, 0x200C, 0x062E, 0x0648}), (CP{0xFEEE, 0xFEA7, 0xFBFD, 0xFEE3}));
}

// ZWJ forces a join where none would occur: a lone beh followed by ZWJ takes
// initial form; preceded by ZWJ it takes final form.
TEST(JoinerShaping, ZwjForcesJoin) {
  EXPECT_EQ(shapeVisual({0x0628, 0x200D}), (CP{0xFE91}));
  EXPECT_EQ(shapeVisual({0x200D, 0x0628}), (CP{0xFE90}));
}

/* ── Mixed-direction text ────────────────────────────────────────────── */

// كتاب abc 123 — RTL paragraph: the Arabic (shaped) ends up rightmost;
// "abc 123" resolves to a single LTR run (rule W7 turns the European
// numerals L after the strong L of "abc") and is placed as one block to
// its left, keeping internal left-to-right order.
TEST(MixedDirection, ArabicLatinNumerals) {
  EXPECT_EQ(shapeVisual({0x0643, 0x062A, 0x0627, 0x0628, ' ', 'a', 'b', 'c', ' ', '1', '2', '3'}),
            (CP{'a', 'b', 'c', ' ', '1', '2', '3', ' ', 0xFE8F, 0xFE8E, 0xFE98, 0xFEDB}));
}

// كتاب ١٢٣ — Arabic-Indic digits keep logical order (leftmost run reads
// ١٢٣, not reversed).
TEST(MixedDirection, ArabicIndicNumerals) {
  EXPECT_EQ(shapeVisual({0x0643, 0x062A, 0x0627, 0x0628, ' ', 0x0661, 0x0662, 0x0663}),
            (CP{0x0661, 0x0662, 0x0663, ' ', 0xFE8F, 0xFE8E, 0xFE98, 0xFEDB}));
}

// Watch-item regression probe for the "random spaces" report from #2398
// testing: spaces must be neither duplicated nor dropped by the pipeline.
TEST(MixedDirection, SpaceCountPreserved) {
  const CP visual = shapeVisual({0x0643, 0x062A, 0x0627, 0x0628, ' ', 0x0642, 0x0644, 0x0645, ' ', 'o', 'k'});
  int spaces = 0;
  for (const uint32_t cp : visual) spaces += (cp == ' ');
  EXPECT_EQ(spaces, 2);
  EXPECT_EQ(visual.size(), 11u);  // 9 letters + 2 spaces, nothing absorbed or invented
}

/* ── Regression: existing behaviour unchanged ────────────────────────── */

// Hebrew reorders but never shapes.
TEST(Regression, HebrewUntouchedByShaper) {
  EXPECT_EQ(shapeVisual({0x05E9, 0x05DC, 0x05D5, 0x05DD}), (CP{0x05DD, 0x05D5, 0x05DC, 0x05E9}));
}

// Pure Latin text passes through unchanged.
TEST(Regression, LatinPassthrough) { EXPECT_EQ(shapeVisual({'h', 'e', 'l', 'l', 'o'}), (CP{'h', 'e', 'l', 'l', 'o'})); }

/* ── isTransparentMark ───────────────────────────────────────────────── */

TEST(TransparentMark, RtlMarksAreTransparent) {
  EXPECT_TRUE(BidiUtils::isTransparentMark(0x05B0));  // Hebrew sheva
  EXPECT_TRUE(BidiUtils::isTransparentMark(0x0591));  // Hebrew accent etnahta
  EXPECT_TRUE(BidiUtils::isTransparentMark(0x064E));  // Arabic fatha
  EXPECT_TRUE(BidiUtils::isTransparentMark(0x0651));  // Arabic shadda
  EXPECT_TRUE(BidiUtils::isTransparentMark(0x0670));  // superscript alef
  EXPECT_TRUE(BidiUtils::isTransparentMark(0x06D6));  // Quranic annotation
}

TEST(TransparentMark, LettersAndLatinMarksAreNot) {
  EXPECT_FALSE(BidiUtils::isTransparentMark(0x0300));  // Latin combining grave — different path
  EXPECT_FALSE(BidiUtils::isTransparentMark(0x0628));  // Arabic beh
  EXPECT_FALSE(BidiUtils::isTransparentMark(0x05D0));  // Hebrew alef
  EXPECT_FALSE(BidiUtils::isTransparentMark(0x0661));  // Arabic-Indic digit
  EXPECT_FALSE(BidiUtils::isTransparentMark('a'));
}

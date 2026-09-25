#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>

#include "BengaliReorder.h"
#include "Utf8.h"

namespace {

// Builds UTF-8 from codepoints so the test does not depend on this file's encoding.
std::string cps(const std::initializer_list<uint32_t> codepoints) {
  std::string out;
  for (const uint32_t cp : codepoints) utf8AppendCodepoint(cp, out);
  return out;
}

constexpr uint32_t KA = 0x0995, TTA = 0x099F, DA = 0x09A6, BA = 0x09AC, LA = 0x09B2, RA = 0x09B0, YA = 0x09AF,
                   DDA = 0x09A1, SSA = 0x09B7, NA = 0x09A8, TA = 0x09A4;
constexpr uint32_t AA = 0x09BE, I = 0x09BF, II = 0x09C0, U = 0x09C1, E = 0x09C7, AI = 0x09C8, O = 0x09CB, AU = 0x09CC,
                   AU_MARK = 0x09D7;
constexpr uint32_t HALANT = 0x09CD, NUKTA = 0x09BC, CANDRABINDU = 0x0981, ZWNJ = 0x200C, II_LETTER = 0x0987;

std::string reorder(const std::string& in) {
  std::string out;
  return bengaliReorderForDisplay(in.c_str(), out) ? out : in;
}

}  // namespace

TEST(BengaliReorder, LeavesTextWithoutBengaliUntouched) {
  std::string out;
  EXPECT_FALSE(bengaliReorderForDisplay("", out));
  EXPECT_FALSE(bengaliReorderForDisplay("hello world", out));
  EXPECT_FALSE(bengaliReorderForDisplay("caf\xC3\xA9", out));
}

TEST(BengaliReorder, LeavesAlreadyVisualSyllablesUntouched) {
  std::string out;
  EXPECT_FALSE(bengaliReorderForDisplay(cps({KA, LA, 0x09AE}).c_str(), out));  // কলম
  EXPECT_FALSE(bengaliReorderForDisplay(cps({KA, AA, LA}).c_str(), out));      // কাল
  EXPECT_FALSE(bengaliReorderForDisplay(cps({KA, U}).c_str(), out));           // কু
}

TEST(BengaliReorder, MovesPreBaseVowelSignsBeforeConsonant) {
  EXPECT_EQ(reorder(cps({KA, I})), cps({I, KA}));
  EXPECT_EQ(reorder(cps({KA, E})), cps({E, KA}));
  EXPECT_EQ(reorder(cps({KA, AI})), cps({AI, KA}));
  // টেলিবই -> ে ট ি ল ব ই
  EXPECT_EQ(reorder(cps({TTA, E, LA, I, BA, II_LETTER})), cps({E, TTA, I, LA, BA, II_LETTER}));
}

TEST(BengaliReorder, SplitsTwoPartVowelsAroundConsonant) {
  EXPECT_EQ(reorder(cps({KA, O})), cps({E, KA, AA}));
  EXPECT_EQ(reorder(cps({KA, AU})), cps({E, KA, AU_MARK}));
  // Already-decomposed ো (ে + া) lands in the same order.
  EXPECT_EQ(reorder(cps({KA, E, AA})), cps({E, KA, AA}));
}

TEST(BengaliReorder, MovesVowelSignBeforeWholeConjunct) {
  // দ্বি -> ি দ ্ ব
  EXPECT_EQ(reorder(cps({DA, HALANT, BA, I})), cps({I, DA, HALANT, BA}));
  // ক্ষে -> ে ক ্ ষ
  EXPECT_EQ(reorder(cps({KA, HALANT, SSA, E})), cps({E, KA, HALANT, SSA}));
}

TEST(BengaliReorder, KeepsRephInFront) {
  // র্কে -> র ্ ে ক
  EXPECT_EQ(reorder(cps({RA, HALANT, KA, E})), cps({RA, HALANT, E, KA}));
}

TEST(BengaliReorder, StopsClusterAtFinalHasantAndZwnj) {
  // ক্‌ষি: the ZWNJ ends the first syllable, so ি only moves before ষ.
  EXPECT_EQ(reorder(cps({KA, HALANT, ZWNJ, SSA, I})), cps({KA, HALANT, ZWNJ, I, SSA}));
  // Word-final hasant: nothing to move.
  std::string out;
  EXPECT_FALSE(bengaliReorderForDisplay(cps({NA, TA, HALANT}).c_str(), out));
}

TEST(BengaliReorder, ComposesNuktaLetters) {
  EXPECT_EQ(reorder(cps({YA, NUKTA})), cps({0x09DF}));
  EXPECT_EQ(reorder(cps({DDA, NUKTA, I})), cps({I, 0x09DC}));
  // তৃতীয় with a decomposed য়
  EXPECT_EQ(reorder(cps({TA, 0x09C3, TA, II, YA, NUKTA})), cps({TA, 0x09C3, TA, II, 0x09DF}));
}

TEST(BengaliReorder, KeepsModifiersAfterTheSyllable) {
  EXPECT_EQ(reorder(cps({KA, E, CANDRABINDU})), cps({E, KA, CANDRABINDU}));
}

TEST(BengaliReorder, PreservesSurroundingText) {
  EXPECT_EQ(reorder("a " + cps({KA, I}) + " b."), "a " + cps({I, KA}) + " b.");
  // A vowel sign with no consonant before it is left in place.
  EXPECT_EQ(reorder(cps({I, ' ', KA, I})), cps({I, ' ', I, KA}));
}

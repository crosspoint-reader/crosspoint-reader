#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>

#include "IndicReorder.h"
#include "IndicScripts.h"
#include "Utf8.h"

namespace {

// Builds UTF-8 from codepoints so the test does not depend on this file's encoding.
std::string cps(const std::initializer_list<uint32_t> codepoints) {
  std::string out;
  for (const uint32_t cp : codepoints) utf8AppendCodepoint(cp, out);
  return out;
}

// Bengali.
constexpr uint32_t KA = 0x0995, TTA = 0x099F, DA = 0x09A6, BA = 0x09AC, LA = 0x09B2, RA = 0x09B0, YA = 0x09AF,
                   DDA = 0x09A1, SSA = 0x09B7, NA = 0x09A8, TA = 0x09A4;
constexpr uint32_t AA = 0x09BE, I = 0x09BF, II = 0x09C0, U = 0x09C1, E = 0x09C7, AI = 0x09C8, O = 0x09CB, AU = 0x09CC,
                   AU_MARK = 0x09D7;
constexpr uint32_t HALANT = 0x09CD, NUKTA = 0x09BC, CANDRABINDU = 0x0981, ZWNJ = 0x200C, II_LETTER = 0x0987;

std::string reorder(const std::string& in) {
  std::string out;
  return indicReorderForDisplay(in.c_str(), out) ? out : in;
}

}  // namespace

TEST(BengaliReorder, LeavesTextWithoutBengaliUntouched) {
  std::string out;
  EXPECT_FALSE(indicReorderForDisplay("", out));
  EXPECT_FALSE(indicReorderForDisplay("hello world", out));
  EXPECT_FALSE(indicReorderForDisplay("caf\xC3\xA9", out));
}

TEST(BengaliReorder, LeavesAlreadyVisualSyllablesUntouched) {
  std::string out;
  EXPECT_FALSE(indicReorderForDisplay(cps({KA, LA, 0x09AE}).c_str(), out));  // কলম
  EXPECT_FALSE(indicReorderForDisplay(cps({KA, AA, LA}).c_str(), out));      // কাল
  EXPECT_FALSE(indicReorderForDisplay(cps({KA, U}).c_str(), out));           // কু
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
  EXPECT_FALSE(indicReorderForDisplay(cps({NA, TA, HALANT}).c_str(), out));
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

TEST(BengaliLineBreaks, NeverSplitASyllable) {
  EXPECT_FALSE(indic::syllableBreakAllowed(KA, I));            // before a vowel sign
  EXPECT_FALSE(indic::syllableBreakAllowed(KA, HALANT));       // before a hasant
  EXPECT_FALSE(indic::syllableBreakAllowed(HALANT, SSA));      // inside a conjunct
  EXPECT_FALSE(indic::syllableBreakAllowed(KA, CANDRABINDU));  // before a mark
  EXPECT_FALSE(indic::syllableBreakAllowed(YA, NUKTA));
  EXPECT_FALSE(indic::syllableBreakAllowed(HALANT, ZWNJ));  // before a joiner
  EXPECT_TRUE(indic::syllableBreakAllowed(I, KA));          // between syllables
  EXPECT_TRUE(indic::syllableBreakAllowed(ZWNJ, SSA));      // after an explicit hasant
  EXPECT_TRUE(indic::syllableBreakAllowed('a', 'b'));
}

TEST(IndicScripts, MapsEachBlockToItsScript) {
  EXPECT_EQ(indic::scriptOf('a'), nullptr);
  EXPECT_EQ(indic::scriptOf(0x08FF), nullptr);
  EXPECT_EQ(indic::scriptOf(0x0E01), nullptr);  // Thai
  for (const indic::ScriptInfo& script : indic::SCRIPTS) {
    EXPECT_EQ(indic::scriptOf(script.first), &script) << script.name;
    EXPECT_EQ(indic::scriptOf(script.last()), &script) << script.name;
    EXPECT_EQ(indic::scriptOf(script.probe), &script) << script.name;
  }
  EXPECT_STREQ(indic::scriptOf(0x0915)->name, "Devanagari");
  EXPECT_STREQ(indic::scriptOf(0x0B95)->name, "Tamil");
  EXPECT_STREQ(indic::scriptOf(0x0D9A)->name, "Sinhala");
}

TEST(IndicScripts, SharedCodepointsBelongToNoScript) {
  for (const uint32_t cp : {0x0951u, 0x0954u, 0x0964u, 0x0965u, indic::ZWNJ, indic::ZWJ, indic::DOTTED_CIRCLE}) {
    EXPECT_TRUE(indic::isShared(cp)) << std::hex << cp;
    EXPECT_EQ(indic::scriptOf(cp), nullptr) << std::hex << cp;
  }
  EXPECT_FALSE(indic::isShared(0x0966));  // Devanagari digit zero is Devanagari's own
}

TEST(IndicScripts, ByteScanFindsExactlyTheIndicBlocks) {
  EXPECT_FALSE(indic::containsIndic(nullptr));
  EXPECT_FALSE(indic::containsIndic("hello"));
  EXPECT_FALSE(indic::containsIndic(cps({0x08FF}).c_str()));
  EXPECT_FALSE(indic::containsIndic(cps({0x0E01}).c_str()));  // Thai
  EXPECT_FALSE(indic::containsIndic(cps({0x4E00}).c_str()));
  EXPECT_TRUE(indic::containsIndic(cps({0x0900}).c_str()));
  EXPECT_TRUE(indic::containsIndic(("a " + cps({0x0B95})).c_str()));
  EXPECT_TRUE(indic::containsIndic(cps({0x0DFF}).c_str()));
}

TEST(IndicScripts, ClassifiesSigns) {
  EXPECT_TRUE(indic::isDependentSign(0x093F));   // Devanagari i
  EXPECT_TRUE(indic::isDependentSign(0x0BCD));   // Tamil pulli
  EXPECT_TRUE(indic::isDependentSign(0x0DDF));   // Sinhala gayanukitta
  EXPECT_FALSE(indic::isDependentSign(0x0915));  // a consonant
  EXPECT_FALSE(indic::isDependentSign(0x0966));  // a digit
  EXPECT_FALSE(indic::isDependentSign(0x0300));  // combining, but not Indic
  EXPECT_TRUE(indic::isVirama(0x094D));
  EXPECT_TRUE(indic::isVirama(0x0D3B));  // Malayalam vertical bar virama
  EXPECT_TRUE(indic::isVirama(0x0DCA));  // Sinhala al-lakuna
  EXPECT_FALSE(indic::isVirama(0x0DCF));
  EXPECT_TRUE(indic::isNukta(0x093C));
  EXPECT_FALSE(indic::isNukta(0x094D));
}

namespace {
// Devanagari.
constexpr uint32_t DEVA_KA = 0x0915, DEVA_SSA = 0x0937, DEVA_RA = 0x0930, DEVA_I = 0x093F, DEVA_II = 0x0940,
                   DEVA_VIRAMA = 0x094D, DEVA_NUKTA = 0x093C;
}  // namespace

TEST(IndicReorder, Devanagari) {
  EXPECT_EQ(reorder(cps({DEVA_KA, DEVA_I})), cps({DEVA_I, DEVA_KA}));  // कि
  EXPECT_EQ(reorder(cps({DEVA_KA, DEVA_VIRAMA, DEVA_SSA, DEVA_I})),    // क्षि
            cps({DEVA_I, DEVA_KA, DEVA_VIRAMA, DEVA_SSA}));
  EXPECT_EQ(reorder(cps({DEVA_RA, DEVA_VIRAMA, DEVA_KA, DEVA_I})),  // र्कि: reph stays first
            cps({DEVA_RA, DEVA_VIRAMA, DEVA_I, DEVA_KA}));
  EXPECT_EQ(reorder(cps({DEVA_KA, DEVA_NUKTA, DEVA_I})), cps({DEVA_I, 0x0958}));  // क़ि
  EXPECT_EQ(reorder(cps({0x092B, DEVA_NUKTA})), cps({0x095E}));                   // फ़
  std::string out;
  EXPECT_FALSE(indicReorderForDisplay(cps({DEVA_KA, DEVA_II}).c_str(), out));  // की: post-base only
}

TEST(IndicReorder, GurmukhiGujaratiAndOdia) {
  EXPECT_EQ(reorder(cps({0x0A15, 0x0A3F})), cps({0x0A3F, 0x0A15}));          // ਕਿ
  EXPECT_EQ(reorder(cps({0x0A1C, 0x0A3C})), cps({0x0A5B}));                  // ਜ਼
  EXPECT_EQ(reorder(cps({0x0A95, 0x0ABF})), cps({0x0ABF, 0x0A95}));          // કિ
  EXPECT_EQ(reorder(cps({0x0B15, 0x0B4B})), cps({0x0B47, 0x0B15, 0x0B3E}));  // କୋ
  EXPECT_EQ(reorder(cps({0x0B15, 0x0B48})), cps({0x0B47, 0x0B15, 0x0B56}));  // କୈ
  EXPECT_EQ(reorder(cps({0x0B21, 0x0B3C})), cps({0x0B5C}));                  // ଡ଼
}

TEST(IndicReorder, TamilKeepsEachPulliSyllableSeparate) {
  constexpr uint32_t TA_KA = 0x0B95, TA_SSA = 0x0BB7, PULLI = 0x0BCD, TA_E = 0x0BC6, TA_AA = 0x0BBE;
  EXPECT_EQ(reorder(cps({TA_KA, TA_E})), cps({TA_E, TA_KA}));            // கெ
  EXPECT_EQ(reorder(cps({TA_KA, 0x0BCA})), cps({TA_E, TA_KA, TA_AA}));   // கொ
  EXPECT_EQ(reorder(cps({TA_KA, 0x0BCC})), cps({TA_E, TA_KA, 0x0BD7}));  // கௌ
  EXPECT_EQ(reorder(cps({TA_KA, PULLI, TA_KA, 0x0BCA})),                 // க்கொ: the pulli is visible,
            cps({TA_KA, PULLI, TA_E, TA_KA, TA_AA}));                    // so ெ moves before the second க
  EXPECT_EQ(reorder(cps({TA_KA, PULLI, indic::ZWJ, TA_SSA, TA_E})),      // க்‍ஷெ: ZWJ asks for a conjunct
            cps({TA_E, TA_KA, PULLI, indic::ZWJ, TA_SSA}));
}

TEST(IndicReorder, MalayalamAndSinhala) {
  EXPECT_EQ(reorder(cps({0x0D15, 0x0D4A})), cps({0x0D46, 0x0D15, 0x0D3E}));  // കൊ
  EXPECT_EQ(reorder(cps({0x0D15, 0x0D4D, 0x0D15, 0x0D46})),                  // ക്കെ
            cps({0x0D46, 0x0D15, 0x0D4D, 0x0D15}));
  constexpr uint32_t SI_KA = 0x0D9A, SI_SSA = 0x0DC2, AL_LAKUNA = 0x0DCA, KOMBUVA = 0x0DD9;
  EXPECT_EQ(reorder(cps({SI_KA, 0x0DDC})), cps({KOMBUVA, SI_KA, 0x0DCF}));             // කො
  EXPECT_EQ(reorder(cps({SI_KA, 0x0DDD})), cps({KOMBUVA, SI_KA, 0x0DCF, AL_LAKUNA}));  // කෝ: three parts
  EXPECT_EQ(reorder(cps({SI_KA, AL_LAKUNA, indic::ZWJ, SI_SSA, KOMBUVA})),             // ක්‍ෂෙ
            cps({KOMBUVA, SI_KA, AL_LAKUNA, indic::ZWJ, SI_SSA}));
  EXPECT_EQ(reorder(cps({SI_KA, AL_LAKUNA, SI_SSA, KOMBUVA})),  // ක්ෂෙ: no ZWJ, no conjunct
            cps({SI_KA, AL_LAKUNA, KOMBUVA, SI_SSA}));
}

TEST(IndicReorder, LeavesScriptsWithoutPreBaseVowelsAlone) {
  std::string out;
  EXPECT_FALSE(indicReorderForDisplay(cps({0x0C15, 0x0C3F}).c_str(), out));  // Telugu కి
  EXPECT_FALSE(indicReorderForDisplay(cps({0x0C95, 0x0CC6}).c_str(), out));  // Kannada ಕೆ
}

TEST(IndicReorder, ReordersEachScriptInMixedText) {
  EXPECT_EQ(reorder(cps({KA, I, ' ', DEVA_KA, DEVA_I})), cps({I, KA, ' ', DEVA_I, DEVA_KA}));
}

TEST(IndicLineBreaks, NeverSplitASyllableInAnyScript) {
  EXPECT_FALSE(indic::syllableBreakAllowed(DEVA_KA, DEVA_I));
  EXPECT_FALSE(indic::syllableBreakAllowed(DEVA_VIRAMA, DEVA_SSA));
  EXPECT_FALSE(indic::syllableBreakAllowed(0x0B95, 0x0BCD));  // before Tamil pulli
  EXPECT_FALSE(indic::syllableBreakAllowed(0x0D15, 0x0D57));  // before Malayalam au length mark
  EXPECT_FALSE(indic::syllableBreakAllowed(0x0DCA, indic::ZWJ));
  EXPECT_FALSE(indic::syllableBreakAllowed(indic::ZWJ, 0x0DC2));
  EXPECT_TRUE(indic::syllableBreakAllowed(DEVA_I, DEVA_KA));
  EXPECT_TRUE(indic::syllableBreakAllowed(0x0C3F, 0x0C15));  // Telugu: after a vowel sign
}

#pragma once

// Shared by the shaping pipeline (OtShaper.cpp, OtNormalize.cpp) and the
// Indic and USE shapers (OtIndic.cpp, OtUse.cpp). Not part of the public API.

#include <cstdint>

#include "OtBuffer.h"
#include "OtCategories.h"
#include "OtFace.h"
#include "OtPlan.h"
#include "OtSyllableMachines.h"

namespace ot {

constexpr uint32_t DOTTED_CIRCLE = 0x25CC;

// Per-codepoint data from OtUnicodeData.h.
struct CharData {
  uint8_t genCat;
  uint8_t ccc;  // modified combining class
  uint8_t indicCategory;
  uint8_t indicPosition;
  uint8_t useCategory;
  bool ignorable;
};
CharData charData(uint32_t cp);

// Sets unicodeProps from the codepoint (_hb_glyph_info_set_unicode_props).
void setUnicodeProps(GlyphInfo& info, Buffer& buffer);

// Normalization and vowel constraints (OtNormalize.cpp).
void preprocessVowelConstraints(Script script, Buffer& buffer);
void normalize(const Face& face, const Plan& plan, Buffer& buffer);

// Calls `f(start, end)` for each syllable (a run of equal syllable bytes).
template <typename F>
void forEachSyllable(const Buffer& buffer, F f) {
  const unsigned len = buffer.len();
  unsigned start = 0;
  while (start < len) {
    unsigned end = start + 1;
    while (end < len && buffer.info[end].syllable == buffer.info[start].syllable) end++;
    f(start, end);
    start = end;
  }
}

// hb_syllabic_insert_dotted_circles(): a dotted circle in each broken
// syllable, after any leading repha. The circle gets `category` and, when
// `setPosition`, `position`.
void insertDottedCircles(const Face& face, Buffer& buffer, uint8_t brokenSyllableType, uint8_t category,
                         uint8_t rephaCategory, bool setPosition, uint8_t position);

// Longest syllable at the start of a category sequence, by one of the
// generated scanners (OtSyllableMachines.h): the Ragel semantics HarfBuzz
// uses, longest match first and the earlier pattern on a tie. `category(i)`
// is the category of the i-th character. Returns the length, 0 when no
// pattern matches (every scanner has a one-character catch-all, so only
// for an empty sequence), and the syllable type.
template <typename Category>
unsigned scanSyllable(const uint8_t* classes, const uint8_t classCount, const uint8_t* transitions,
                      const uint8_t* accept, const unsigned available, Category category, uint8_t* type) {
  unsigned state = 0;
  unsigned best = 0;
  for (unsigned i = 0; i < available; i++) {
    const uint8_t c = category(i);
    state = transitions[state * classCount + (c < machines::CATEGORY_COUNT ? classes[c] : 0)];
    if (state == machines::NO_TRANSITION) break;
    if (accept[state] != machines::NO_ACCEPT) {
      best = i + 1;
      *type = accept[state];
    }
  }
  return best;
}

// The Indic shaper (hb-ot-shaper-indic.cc).
void indicSetupMasks(Buffer& buffer);
void indicSetupSyllables(Buffer& buffer);
void indicInitialReordering(const Face& face, const Plan& plan, Buffer& buffer);
void indicFinalReordering(const Face& face, const Plan& plan, Buffer& buffer);

// The Universal Shaping Engine (hb-ot-shaper-use.cc).
void useSetupMasks(Buffer& buffer);
void useSetupSyllables(const Plan& plan, Buffer& buffer);
void useRecordRphf(const Plan& plan, Buffer& buffer);
void useRecordPref(Buffer& buffer);
void useReorder(const Face& face, Buffer& buffer);

}  // namespace ot

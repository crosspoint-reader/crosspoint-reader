#include <algorithm>

#include "OtShaperInternal.h"
#include "OtSort.h"

// The Indic shaper: syllable segmentation, then the initial and final
// reordering around the basic-shaping GSUB features. A port of HarfBuzz's
// hb-ot-shaper-indic.cc and hb-ot-shaper-indic-machine.rl; see those for the
// history behind each rule (Uniscribe compatibility notes, bug links).

namespace ot {

namespace {

constexpr uint64_t CONSONANT_FLAGS = flag(icat::C) | flag(icat::CS) | flag(icat::Ra) | flag(icat::CM) | flag(icat::V) |
                                     flag(icat::PLACEHOLDER) | flag(icat::DOTTEDCIRCLE);
constexpr uint64_t JOINER_FLAGS = flag(icat::ZWJ) | flag(icat::ZWNJ);

bool isOneOf(const GlyphInfo& info, const uint64_t flags) {
  if (info.ligated()) return false;  // if it ligated, all bets are off
  return (flags >> info.category) & 1;
}
bool isConsonant(const GlyphInfo& info) { return isOneOf(info, CONSONANT_FLAGS); }
bool isJoiner(const GlyphInfo& info) { return isOneOf(info, JOINER_FLAGS); }
bool isHalant(const GlyphInfo& info) { return isOneOf(info, flag(icat::H)); }
bool flagSet(const uint8_t value, const uint64_t flags) { return (flags >> value) & 1; }

// consonant_position_from_face().
uint8_t consonantPositionFromFace(const Face& face, const Plan& plan, const uint32_t consonant, const uint32_t virama) {
  const uint32_t glyphs[3] = {virama, consonant, virama};
  if (plan.wouldSubstitute(face, Plan::WS_BLWF, glyphs, 2) ||
      plan.wouldSubstitute(face, Plan::WS_BLWF, glyphs + 1, 2) ||
      plan.wouldSubstitute(face, Plan::WS_VATU, glyphs, 2) ||
      plan.wouldSubstitute(face, Plan::WS_VATU, glyphs + 1, 2)) {
    return ipos::BELOW_C;
  }
  if (plan.wouldSubstitute(face, Plan::WS_PSTF, glyphs, 2) ||
      plan.wouldSubstitute(face, Plan::WS_PSTF, glyphs + 1, 2)) {
    return ipos::POST_C;
  }
  if (plan.wouldSubstitute(face, Plan::WS_PREF, glyphs, 2) ||
      plan.wouldSubstitute(face, Plan::WS_PREF, glyphs + 1, 2)) {
    return ipos::POST_C;
  }
  return ipos::BASE_C;
}

// The font's virama glyph, 0 when it has none.
uint32_t viramaGlyph(const Face& face, const Plan& plan) {
  uint32_t glyph;
  face.nominalGlyph(plan.indicConfig->virama, &glyph);
  return glyph;
}

// Initial reordering step 1: the base consonant, searching back from the
// end past consonants with below- or post-base forms, and whether the
// syllable starts with a reph.
unsigned findBase(const Face& face, const Plan& plan, Buffer& buffer, const unsigned start, const unsigned end,
                  bool* hasRephOut) {
  auto& info = buffer.info;
  const IndicConfig& config = *plan.indicConfig;
  const uint32_t* masks = plan.indicMasks;
  unsigned base = end;
  bool hasReph = false;
  unsigned limit = start;
  if (masks[Plan::RPHF] && start + 3 <= end &&
      ((config.rephMode == IndicConfig::REPH_IMPLICIT && !isJoiner(info[start + 2])) ||
       (config.rephMode == IndicConfig::REPH_EXPLICIT && info[start + 2].category == icat::ZWJ))) {
    const uint32_t glyphs[3] = {info[start].codepoint, info[start + 1].codepoint,
                                config.rephMode == IndicConfig::REPH_EXPLICIT ? info[start + 2].codepoint : 0u};
    if (plan.wouldSubstitute(face, Plan::WS_RPHF, glyphs, 2) ||
        (config.rephMode == IndicConfig::REPH_EXPLICIT && plan.wouldSubstitute(face, Plan::WS_RPHF, glyphs, 3))) {
      limit += 2;
      while (limit < end && isJoiner(info[limit])) limit++;
      base = start;
      hasReph = true;
    }
  } else if (config.rephMode == IndicConfig::REPH_LOG_REPHA && info[start].category == icat::Repha) {
    limit += 1;
    while (limit < end && isJoiner(info[limit])) limit++;
    base = start;
    hasReph = true;
  }

  {
    unsigned i = end;
    bool seenBelow = false;
    do {
      i--;
      if (isConsonant(info[i])) {
        if (info[i].position != ipos::BELOW_C && (info[i].position != ipos::POST_C || seenBelow)) {
          base = i;
          break;
        }
        if (info[i].position == ipos::BELOW_C) seenBelow = true;
        base = i;
      } else if (start < i && info[i].category == icat::ZWJ && info[i - 1].category == icat::H) {
        // A ZWJ after a halant stops the search and asks for a half form.
        break;
      }
    } while (i > limit);
  }

  if (hasReph && base == start && limit - base <= 2) hasReph = false;  // no other consonant: Ra is the base
  *hasRephOut = hasReph;
  return base;
}

// Steps 2 and 3 (matras were decomposed and marks ordered by
// normalization): each glyph's position, so sorting puts it in visual order.
void assignPositions(const Plan& plan, Buffer& buffer, const unsigned start, const unsigned end, const unsigned base,
                     const bool hasReph) {
  auto& info = buffer.info;
  for (unsigned i = start; i < base; i++) info[i].position = std::min<uint8_t>(ipos::PRE_C, info[i].position);
  if (base < end) info[base].position = ipos::BASE_C;
  if (hasReph) info[start].position = ipos::RA_TO_BECOME_REPH;

  // Old-spec fonts: move the first post-base halant after the last consonant.
  if (plan.isOldSpec) {
    const bool disallowDoubleHalants = plan.script == Script::Kannada;
    for (unsigned i = base + 1; i < end; i++) {
      if (info[i].category != icat::H) continue;
      unsigned j;
      for (j = end - 1; j > i; j--) {
        if (isConsonant(info[j]) || (disallowDoubleHalants && info[j].category == icat::H)) break;
      }
      if (info[j].category != icat::H && j > i) buffer.moveGlyph(i, j);
      break;
    }
  }

  // Attach misc marks to the previous character to move with it.
  {
    uint8_t lastPos = ipos::START;
    for (unsigned i = start; i < end; i++) {
      if (flagSet(info[i].category, JOINER_FLAGS | flag(icat::N) | flag(icat::RS) | flag(icat::CM) | flag(icat::H))) {
        info[i].position = lastPos;
        if (info[i].category == icat::H && info[i].position == ipos::PRE_M) {
          // Uniscribe doesn't move the halant with a left matra.
          for (unsigned j = i; j > start; j--) {
            if (info[j - 1].position != ipos::PRE_M) {
              info[i].position = info[j - 1].position;
              break;
            }
          }
        }
      } else if (info[i].position != ipos::SMVD) {
        if (info[i].category == icat::MPst && i > start && info[i - 1].category == icat::SM) {
          info[i - 1].position = info[i].position;
        }
        lastPos = info[i].position;
      }
    }
  }
  // Post-base consonants own anything before them since the last consonant or matra.
  {
    unsigned last = base;
    for (unsigned i = base + 1; i < end; i++) {
      if (isConsonant(info[i])) {
        for (unsigned j = last + 1; j < i; j++) {
          if (info[j].position < ipos::SMVD) info[j].position = info[i].position;
        }
        last = i;
      } else if (flagSet(info[i].category, flag(icat::M) | flag(icat::MPst))) {
        last = i;
      }
    }
  }
}

// Sorts the syllable by position; returns the base's new index. A run of
// left matras is flipped back into logical order.
unsigned sortSyllable(Buffer& buffer, const unsigned start, const unsigned end) {
  auto& info = buffer.info;
  stableSort(info.data() + start, info.data() + end,
             [](const GlyphInfo& a, const GlyphInfo& b) { return a.position < b.position; });

  // Find the base again; also flip a sequence of left matras.
  unsigned firstLeftMatra = end;
  unsigned lastLeftMatra = end;
  unsigned base = end;
  for (unsigned i = start; i < end; i++) {
    if (info[i].position == ipos::BASE_C) {
      base = i;
      break;
    }
    if (info[i].position == ipos::PRE_M) {
      if (firstLeftMatra == end) firstLeftMatra = i;
      lastLeftMatra = i;
    }
  }
  if (firstLeftMatra < lastLeftMatra) {
    buffer.reverseRange(firstLeftMatra, lastLeftMatra + 1);
    // Reverse back nuktas, etc.
    unsigned i = firstLeftMatra;
    for (unsigned j = i; j <= lastLeftMatra; j++) {
      if (flagSet(info[j].category, flag(icat::M) | flag(icat::MPst))) {
        buffer.reverseRange(i, j + 1);
        i = j + 1;
      }
    }
  }
  return base;
}

// Feature masks: reph, half and below forms before the base, below/above/
// post forms after it, pre-base-reordering Ra, and ZWNJ blocking half forms.
void setMasks(const Face& face, const Plan& plan, Buffer& buffer, const unsigned start, const unsigned end,
              const unsigned base) {
  auto& info = buffer.info;
  const IndicConfig& config = *plan.indicConfig;
  const uint32_t* masks = plan.indicMasks;
  for (unsigned i = start; i < end && info[i].position == ipos::RA_TO_BECOME_REPH; i++) {
    info[i].mask |= masks[Plan::RPHF];
  }
  {
    uint32_t mask = masks[Plan::HALF];
    if (!plan.isOldSpec && config.blwfMode == IndicConfig::BLWF_PRE_AND_POST) mask |= masks[Plan::BLWF];
    for (unsigned i = start; i < base; i++) info[i].mask |= mask;
    mask = masks[Plan::BLWF] | masks[Plan::ABVF] | masks[Plan::PSTF];
    for (unsigned i = base + 1; i < end; i++) info[i].mask |= mask;
  }

  if (plan.isOldSpec && plan.script == Script::Devanagari) {
    // Old-spec eyelash Ra.
    for (unsigned i = start; i + 1 < base; i++) {
      if (info[i].category == icat::Ra && info[i + 1].category == icat::H &&
          (i + 2 == base || info[i + 2].category != icat::ZWJ)) {
        info[i].mask |= masks[Plan::BLWF];
        info[i + 1].mask |= masks[Plan::BLWF];
      }
    }
  }

  constexpr unsigned PREF_LEN = 2;
  if (masks[Plan::PREF] && base + PREF_LEN < end) {
    // A Halant,Ra sequence to reorder before the base.
    for (unsigned i = base + 1; i + PREF_LEN - 1 < end; i++) {
      const uint32_t glyphs[2] = {info[i].codepoint, info[i + 1].codepoint};
      if (plan.wouldSubstitute(face, Plan::WS_PREF, glyphs, PREF_LEN)) {
        for (unsigned j = 0; j < PREF_LEN; j++) info[i++].mask |= masks[Plan::PREF];
        break;
      }
    }
  }

  // ZWNJ disables HALF on what precedes it.
  for (unsigned i = start + 1; i < end; i++) {
    if (!isJoiner(info[i])) continue;
    const bool nonJoiner = info[i].category == icat::ZWNJ;
    unsigned j = i;
    do {
      j--;
      if (nonJoiner) info[j].mask &= ~masks[Plan::HALF];
    } while (j > start && !isConsonant(info[j]));
  }
}

// initial_reordering_consonant_syllable().
void initialReorderingConsonantSyllable(const Face& face, const Plan& plan, Buffer& buffer, const unsigned start,
                                        const unsigned end) {
  auto& info = buffer.info;

  // Kannada Ra,H,ZWJ behaves like Ra,ZWJ,H (legacy usage).
  if (plan.script == Script::Kannada && start + 3 <= end && isOneOf(info[start], flag(icat::Ra)) &&
      isOneOf(info[start + 1], flag(icat::H)) && isOneOf(info[start + 2], flag(icat::ZWJ))) {
    std::swap(info[start + 1], info[start + 2]);
  }

  bool hasReph;
  const unsigned base = findBase(face, plan, buffer, start, end, &hasReph);
  assignPositions(plan, buffer, start, end, base, hasReph);
  setMasks(face, plan, buffer, start, end, sortSyllable(buffer, start, end));
}

void finalReorderingSyllable(const Plan& plan, Buffer& buffer, const uint32_t virama, const unsigned start,
                             const unsigned end) {
  auto& info = buffer.info;
  const IndicConfig& config = *plan.indicConfig;
  const uint32_t* masks = plan.indicMasks;
  const bool malayalamOrTamil = plan.script == Script::Malayalam || plan.script == Script::Tamil;

  // Recover a halant whose category ligation and multiplication lost.
  if (virama) {
    for (unsigned i = start; i < end; i++) {
      if (info[i].codepoint == virama && info[i].ligated() && info[i].multiplied()) {
        info[i].category = icat::H;
        info[i].glyphProps &= ~(props::LIGATED | props::MULTIPLIED);
      }
    }
  }

  bool tryPref = masks[Plan::PREF] != 0;

  // Find the base again.
  unsigned base;
  for (base = start; base < end; base++) {
    if (info[base].position >= ipos::BASE_C) {
      if (tryPref && base + 1 < end) {
        for (unsigned i = base + 1; i < end; i++) {
          if ((info[i].mask & masks[Plan::PREF]) != 0) {
            if (!(info[i].substituted() && info[i].ligatedAndDidntMultiply())) {
              // A pref candidate that formed nothing: the base is around here.
              base = i;
              while (base < end && isHalant(info[base])) base++;
              if (base < end) info[base].position = ipos::BASE_C;
              tryPref = false;
            }
            break;
          }
        }
        if (base == end) break;
      }
      // Malayalam: skip over unformed below- (but not post-) forms.
      if (plan.script == Script::Malayalam) {
        for (unsigned i = base + 1; i < end; i++) {
          while (i < end && isJoiner(info[i])) i++;
          if (i == end || !isHalant(info[i])) break;
          i++;
          while (i < end && isJoiner(info[i])) i++;
          if (i < end && isConsonant(info[i]) && info[i].position == ipos::BELOW_C) {
            base = i;
            info[base].position = ipos::BASE_C;
          }
        }
      }
      if (start < base && info[base].position > ipos::BASE_C) base--;
      break;
    }
  }
  if (base == end && start < base && isOneOf(info[base - 1], flag(icat::ZWJ))) base--;
  if (base < end) {
    while (start < base && isOneOf(info[base], flag(icat::N) | flag(icat::H))) base--;
  }

  // Reorder a pre-base matra closer to the main consonant.
  if (start + 1 < end && start < base) {
    unsigned newPos = base == end ? base - 2 : base - 1;
    if (!malayalamOrTamil) {
      for (;;) {
        while (newPos > start && !isOneOf(info[newPos], flag(icat::M) | flag(icat::MPst) | flag(icat::H))) newPos--;
        if (isHalant(info[newPos]) && info[newPos].position != ipos::PRE_M) {
          // A ZWJ after the halant: keep searching.
          if (newPos + 1 < end && info[newPos + 1].category == icat::ZWJ && newPos > start) {
            newPos--;
            continue;
          }
        } else {
          newPos = start;  // no move
        }
        break;
      }
    }
    if (start < newPos && info[newPos].position != ipos::PRE_M) {
      for (unsigned i = newPos; i > start; i--) {
        if (info[i - 1].position == ipos::PRE_M) {
          const unsigned oldPos = i - 1;
          if (oldPos < base && base <= newPos) base--;
          buffer.moveGlyph(oldPos, newPos);
          newPos--;
        }
      }
    }
  }

  // Reorder reph.
  if (start + 1 < end && info[start].position == ipos::RA_TO_BECOME_REPH &&
      ((info[start].category == icat::Repha) ^ info[start].ligatedAndDidntMultiply())) {
    unsigned newRephPos = 0;
    const uint8_t rephPos = config.rephPos;
    bool found = false;

    // Step 2: after the first explicit halant between the first post-reph
    // consonant and the last main consonant.
    auto afterExplicitHalant = [&]() {
      newRephPos = start + 1;
      while (newRephPos < base && !isHalant(info[newRephPos])) newRephPos++;
      if (newRephPos < base && isHalant(info[newRephPos])) {
        if (newRephPos + 1 < base && isJoiner(info[newRephPos + 1])) newRephPos++;
        return true;
      }
      return false;
    };

    if (rephPos != ipos::AFTER_POST) {
      found = afterExplicitHalant();
      // Step 3: after the main consonant.
      if (!found && rephPos == ipos::AFTER_MAIN) {
        newRephPos = base;
        while (newRephPos + 1 < end && info[newRephPos + 1].position <= ipos::AFTER_MAIN) newRephPos++;
        found = newRephPos < end;
      }
      // Step 4: before the first post-base consonant.
      if (!found && rephPos == ipos::AFTER_SUB) {
        newRephPos = base;
        while (newRephPos + 1 < end && !flagSet(info[newRephPos + 1].position,
                                                flag(ipos::POST_C) | flag(ipos::AFTER_POST) | flag(ipos::SMVD))) {
          newRephPos++;
        }
        found = newRephPos < end;
      }
    }
    // Step 5: same as step 2.
    if (!found) found = afterExplicitHalant();
    // Step 6: the end of the syllable.
    if (!found) {
      newRephPos = end - 1;
      while (newRephPos > start && info[newRephPos].position == ipos::SMVD) newRephPos--;
      // Before a trailing halant when a matra precedes it, so they interact.
      if (isHalant(info[newRephPos])) {
        for (unsigned i = base + 1; i < newRephPos; i++) {
          if (flagSet(info[i].category, flag(icat::M) | flag(icat::MPst))) newRephPos--;
        }
      }
    }

    buffer.moveGlyph(start, newRephPos);
    if (start < base && base <= newRephPos) base--;
  }

  // Reorder a pre-base-reordering consonant.
  if (tryPref && base + 1 < end) {
    for (unsigned i = base + 1; i < end; i++) {
      if ((info[i].mask & masks[Plan::PREF]) == 0) continue;
      // Only if it ligated (the pref form formed).
      if (info[i].ligatedAndDidntMultiply()) {
        unsigned newPos = base;
        if (!malayalamOrTamil) {
          while (newPos > start && !isOneOf(info[newPos - 1], flag(icat::M) | flag(icat::MPst) | flag(icat::H)))
            newPos--;
        }
        if (newPos > start && isHalant(info[newPos - 1])) {
          if (newPos < end && isJoiner(info[newPos])) newPos++;
        }
        const unsigned oldPos = i;
        buffer.moveGlyph(oldPos, newPos);
        if (newPos <= base && base < oldPos) base++;
      }
      break;
    }
  }

  // 'init' for a left matra at the start of a word.
  if (info[start].position == ipos::PRE_M) {
    if (!start || !(info[start - 1].genCat() >= gc::FORMAT && info[start - 1].genCat() <= gc::NON_SPACING_MARK)) {
      info[start].mask |= masks[Plan::INIT];
    }
  }
}

}  // namespace

void indicSetupMasks(Buffer& buffer) {
  for (GlyphInfo& g : buffer.info) {
    const CharData d = charData(g.codepoint);
    g.category = d.indicCategory;
    g.position = d.indicPosition;
  }
}

void indicSetupSyllables(Buffer& buffer) {
  using namespace machines;
  const unsigned len = buffer.len();
  unsigned serial = 1;
  unsigned p = 0;
  while (p < len) {
    uint8_t found = isyl::NON_INDIC;
    unsigned length = scanSyllable(
        INDIC_CLASSES, INDIC_CLASS_COUNT, INDIC_TRANSITIONS, INDIC_ACCEPT, len - p,
        [&](const unsigned i) { return buffer.info[p + i].category; }, &found);
    if (length == 0) length = 1;
    const uint8_t type = found;
    if (type == isyl::BROKEN) buffer.hasBrokenSyllable = true;
    for (unsigned i = p; i < p + length; i++) buffer.info[i].syllable = static_cast<uint8_t>((serial << 4) | type);
    serial++;
    if (serial == 16) serial = 1;
    p += length;
  }
}

void indicInitialReordering(const Face& face, const Plan& plan, Buffer& buffer) {
  // update_consonant_positions_indic().
  if (const uint32_t virama = viramaGlyph(face, plan)) {
    for (GlyphInfo& g : buffer.info) {
      if (g.position == ipos::BASE_C) g.position = consonantPositionFromFace(face, plan, g.codepoint, virama);
    }
  }
  insertDottedCircles(face, buffer, isyl::BROKEN, icat::DOTTEDCIRCLE, icat::Repha, true, ipos::END);
  if (!buffer.successful) return;
  forEachSyllable(buffer, [&](const unsigned start, const unsigned end) {
    const uint8_t type = buffer.info[start].syllableType();
    if (type == isyl::VOWEL || type == isyl::CONSONANT || type == isyl::BROKEN || type == isyl::STANDALONE) {
      initialReorderingConsonantSyllable(face, plan, buffer, start, end);
    }
  });
}

void indicFinalReordering(const Face& face, const Plan& plan, Buffer& buffer) {
  if (!buffer.len()) return;
  const uint32_t virama = viramaGlyph(face, plan);
  forEachSyllable(buffer, [&](const unsigned start, const unsigned end) {
    finalReorderingSyllable(plan, buffer, virama, start, end);
  });
}

}  // namespace ot

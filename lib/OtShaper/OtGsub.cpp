#include "OtLayoutInternal.h"

// GSUB subtables (OT/Layout/GSUB), and whether a lookup would substitute a
// glyph sequence (hb_ot_layout_lookup_would_substitute), which the Indic
// shaper uses to find base consonants and reph.

namespace ot::layout {

namespace {

bool applySingleSubst(ApplyContext* c, const Table& st) {
  const uint32_t glyph = c->buffer.cur().codepoint;
  const int index = coverageIndex(st.offset16(2), glyph);
  if (index == NOT_COVERED) return false;
  switch (st.u16(0)) {
    case 1:
      c->replaceGlyph((glyph + st.u16(4)) & 0xFFFF);
      return true;
    case 2:
      if (static_cast<unsigned>(index) >= st.u16(4)) return false;
      c->replaceGlyph(st.u16(6 + 2 * index));
      return true;
    default:
      return false;
  }
}

bool applyMultipleSubst(ApplyContext* c, const Table& st) {
  if (st.u16(0) != 1) return false;
  Buffer& buffer = c->buffer;
  const int index = coverageIndex(st.offset16(2), buffer.cur().codepoint);
  if (index == NOT_COVERED) return false;
  // A sequence index past the array reads as HarfBuzz's empty Null sequence.
  const Table sequence = static_cast<unsigned>(index) < st.u16(4) ? st.offset16(6 + 2 * index) : Table();
  const uint16_t count = sequence.u16(0);
  if (count == 1) {
    c->replaceGlyph(sequence.u16(2));
    return true;
  }
  if (count == 0) {
    buffer.removeGlyph(buffer.idx);
    return true;
  }
  if (!buffer.canGrow(count - 1u)) return false;
  const uint16_t klass = buffer.cur().isLigature() ? props::BASE_GLYPH : 0;
  GlyphInfo working = buffer.cur();
  const unsigned ligId = working.ligId();
  const unsigned at = buffer.idx;
  for (unsigned i = 0; i < count; i++) {
    const uint32_t glyph = sequence.u16(2 + 2 * i);
    if (!ligId) working.setLigPropsForMark(0, i);
    c->setGlyphClass(working, glyph, klass, false, true);
    GlyphInfo out = working;
    out.codepoint = glyph;
    if (i == 0) {
      buffer.info[at] = out;
    } else {
      buffer.info.insert(buffer.info.begin() + at + i, out);
    }
  }
  buffer.idx = at + count;
  return true;
}

bool applyAlternateSubst(ApplyContext* c, const Table& st) {
  if (st.u16(0) != 1) return false;
  Buffer& buffer = c->buffer;
  const int index = coverageIndex(st.offset16(2), buffer.cur().codepoint);
  if (index == NOT_COVERED || static_cast<unsigned>(index) >= st.u16(4)) return false;
  const Table set = st.offset16(6 + 2 * index);
  const uint16_t count = set.u16(0);
  if (!count) return false;
  const uint32_t lookupMask = c->lookupMask;
  const unsigned shift = static_cast<unsigned>(__builtin_ctz(lookupMask));
  unsigned altIndex = (lookupMask & buffer.cur().mask) >> shift;
  if (altIndex == MAX_FEATURE_VALUE && c->random) altIndex = c->randomNumber() % count + 1;
  if (altIndex > count || altIndex == 0) return false;
  c->replaceGlyph(set.u16(2 + 2 * (altIndex - 1)));
  return true;
}

bool applyLigatureSubst(ApplyContext* c, const Table& st) {
  if (st.u16(0) != 1) return false;
  Buffer& buffer = c->buffer;
  const int index = coverageIndex(st.offset16(2), buffer.cur().codepoint);
  if (index == NOT_COVERED || static_cast<unsigned>(index) >= st.u16(4)) return false;
  const Table set = st.offset16(6 + 2 * index);
  const uint16_t ligCount = set.u16(0);
  for (uint16_t i = 0; i < ligCount; i++) {
    const Table lig = set.offset16(2 + 2 * i);
    const uint16_t ligGlyph = lig.u16(0);
    const uint16_t count = lig.u16(2);
    if (!count) continue;
    if (count == 1) {
      c->replaceGlyph(ligGlyph);
      return true;
    }
    if (count > MAX_CONTEXT_LENGTH) continue;
    unsigned matchEnd = 0;
    unsigned totalComponentCount = 0;
    if (!matchInput(c, count, Values{lig, 4}, MatchFunc::Glyph, Table(), &matchEnd, &totalComponentCount)) continue;
    ligateInput(c, count, ligGlyph, totalComponentCount);
    return true;
  }
  return false;
}

bool applyReverseChainSingleSubst(ApplyContext* c, const Table& st) {
  if (st.u16(0) != 1) return false;
  if (c->nestingLeft != MAX_NESTING_LEVEL) return false;  // no chaining to this type
  const int index = coverageIndex(st.offset16(2), c->buffer.cur().codepoint);
  if (index == NOT_COVERED) return false;
  const uint16_t backtrackCount = st.u16(4);
  const uint32_t lookaheadOffset = 6 + 2u * backtrackCount;
  const uint16_t lookaheadCount = st.u16(lookaheadOffset);
  const uint32_t substOffset = lookaheadOffset + 2 + 2u * lookaheadCount;
  if (static_cast<unsigned>(index) >= st.u16(substOffset)) return false;
  if (matchBacktrack(c, backtrackCount, Values{st, 6}, MatchFunc::Coverage, st) &&
      matchLookahead(c, lookaheadCount, Values{st, lookaheadOffset + 2}, MatchFunc::Coverage, st, c->buffer.idx + 1)) {
    c->replaceGlyphInplace(st.u16(substOffset + 2 + 2 * index));
    return true;
  }
  return false;
}

// --- would_apply ---------------------------------------------------------------

struct WouldApply {
  const uint32_t* glyphs;
  unsigned len;
  bool zeroContext;
};

bool wouldMatchValue(const MatchFunc func, const Table& data, const uint32_t glyph, const uint32_t value) {
  switch (func) {
    case MatchFunc::Glyph:
      return glyph == value;
    case MatchFunc::Class:
      return classOf(data, glyph) == value;
    case MatchFunc::Coverage:
      return coverageIndex(data.at(value), glyph) != NOT_COVERED;
    default:
      return true;
  }
}

bool wouldMatchInput(const WouldApply& w, const unsigned count, const Values input, const MatchFunc func,
                     const Table& data) {
  if (count != w.len) return false;
  for (unsigned i = 1; i < count; i++) {
    if (!wouldMatchValue(func, data, w.glyphs[i], input.t.u16(input.offset + 2 * (i - 1)))) return false;
  }
  return true;
}

bool wouldApplyRuleSet(const WouldApply& w, const Table& ruleSet, const MatchFunc func, const Table& data,
                       const bool chain) {
  const uint16_t count = ruleSet.u16(0);
  for (uint16_t i = 0; i < count; i++) {
    const Table rule = ruleSet.offset16(2 + 2 * i);
    if (!chain) {
      if (wouldMatchInput(w, rule.u16(0), Values{rule, 4}, func, data)) return true;
      continue;
    }
    const uint16_t backtrackCount = rule.u16(0);
    uint32_t o = 2 + 2u * backtrackCount;
    const uint16_t inputCount = rule.u16(o);
    const Values input{rule, o + 2};
    o += 2 + 2u * (inputCount ? inputCount - 1u : 0u);
    const uint16_t lookaheadCount = rule.u16(o);
    if ((!w.zeroContext || (!backtrackCount && !lookaheadCount)) && wouldMatchInput(w, inputCount, input, func, data)) {
      return true;
    }
  }
  return false;
}

bool wouldApplySubtable(const WouldApply& w, uint16_t type, Table st) {
  resolveExtension(0, type, st);
  if (st.empty()) return false;
  const uint32_t first = w.glyphs[0];
  switch (type) {
    case gsub::SINGLE:
    case gsub::MULTIPLE:
    case gsub::ALTERNATE:
    case gsub::REVERSE_CHAIN:
      return w.len == 1 && coverageIndex(st.offset16(2), first) != NOT_COVERED;
    case gsub::LIGATURE: {
      const int index = coverageIndex(st.offset16(2), first);
      if (index == NOT_COVERED || static_cast<unsigned>(index) >= st.u16(4)) return false;
      const Table set = st.offset16(6 + 2 * index);
      const uint16_t count = set.u16(0);
      for (uint16_t i = 0; i < count; i++) {
        const Table lig = set.offset16(2 + 2 * i);
        if (w.len != lig.u16(2)) continue;
        bool all = true;
        for (unsigned k = 1; k < w.len && all; k++) all = w.glyphs[k] == lig.u16(4 + 2 * (k - 1));
        if (all) return true;
      }
      return false;
    }
    case gsub::CONTEXT:
      switch (st.u16(0)) {
        case 1: {
          const int index = coverageIndex(st.offset16(2), first);
          if (index == NOT_COVERED || static_cast<unsigned>(index) >= st.u16(4)) return false;
          return wouldApplyRuleSet(w, st.offset16(6 + 2 * index), MatchFunc::Glyph, Table(), false);
        }
        case 2: {
          const Table classDef = st.offset16(4);
          const uint16_t klass = classOf(classDef, first);
          if (klass >= st.u16(6)) return false;
          return wouldApplyRuleSet(w, st.offset16(8 + 2 * klass), MatchFunc::Class, classDef, false);
        }
        case 3:
          return wouldMatchInput(w, st.u16(2), Values{st, 8}, MatchFunc::Coverage, st);
        default:
          return false;
      }
    case gsub::CHAIN:
      switch (st.u16(0)) {
        case 1: {
          const int index = coverageIndex(st.offset16(2), first);
          if (index == NOT_COVERED || static_cast<unsigned>(index) >= st.u16(4)) return false;
          return wouldApplyRuleSet(w, st.offset16(6 + 2 * index), MatchFunc::Glyph, Table(), true);
        }
        case 2: {
          const Table inputClassDef = st.offset16(6);
          const uint16_t klass = classOf(inputClassDef, first);
          if (klass >= st.u16(10)) return false;
          return wouldApplyRuleSet(w, st.offset16(12 + 2 * klass), MatchFunc::Class, inputClassDef, true);
        }
        case 3: {
          const uint16_t backtrackCount = st.u16(2);
          uint32_t o = 4 + 2u * backtrackCount;
          const uint16_t inputCount = st.u16(o);
          const uint32_t inputArray = o + 2;
          o += 2 + 2u * inputCount;
          const uint16_t lookaheadCount = st.u16(o);
          return (!w.zeroContext || (!backtrackCount && !lookaheadCount)) &&
                 wouldMatchInput(w, inputCount, Values{st, inputArray + 2}, MatchFunc::Coverage, st);
        }
        default:
          return false;
      }
    default:
      return false;
  }
}

}  // namespace

bool applyGsubSubtable(ApplyContext* c, const uint16_t type, const Table& st) {
  switch (type) {
    case gsub::SINGLE:
      return applySingleSubst(c, st);
    case gsub::MULTIPLE:
      return applyMultipleSubst(c, st);
    case gsub::ALTERNATE:
      return applyAlternateSubst(c, st);
    case gsub::LIGATURE:
      return applyLigatureSubst(c, st);
    case gsub::CONTEXT:
      return applyContext(c, st);
    case gsub::CHAIN:
      return applyChainContext(c, st);
    case gsub::REVERSE_CHAIN:
      return applyReverseChainSingleSubst(c, st);
    default:
      return false;
  }
}

bool wouldApplyGsubSubtable(const uint32_t* glyphs, const unsigned count, const bool zeroContext, const uint16_t type,
                            const Table st) {
  return wouldApplySubtable(WouldApply{glyphs, count, zeroContext}, type, st);
}

}  // namespace ot::layout

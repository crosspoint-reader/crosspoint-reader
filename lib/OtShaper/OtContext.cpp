#include <cstring>

#include "OtLayoutInternal.h"

// Context matching: input, backtrack and lookahead sequences, ligation, and
// the context and chained-context lookups that run nested lookups at the
// positions they match (match_input, ligate_input, apply_lookup and the
// (Chain)Context formats in hb-ot-layout-gsubgpos.hh).

namespace ot::layout {

bool matchInput(ApplyContext* c, const unsigned count, const Values& input, const MatchFunc func, const Table& data,
                unsigned* endPosition, unsigned* totalComponentCount) {
  Buffer& buffer = c->buffer;
  if (count == 1) {
    *endPosition = buffer.idx + 1;
    c->matchPositions[0] = static_cast<uint16_t>(buffer.idx);
    if (totalComponentCount) *totalComponentCount = buffer.cur().ligNumComps();
    return true;
  }
  if (count > MAX_CONTEXT_LENGTH) return false;

  SkippyIter& it = c->iterInput;
  it.reset(buffer.idx);
  it.setMatch(func, data, input);

  unsigned total = 0;
  const unsigned firstLigId = buffer.cur().ligId();
  const unsigned firstLigComp = buffer.cur().ligComp();
  enum { LIGBASE_NOT_CHECKED, LIGBASE_MAY_NOT_SKIP, LIGBASE_MAY_SKIP } ligbase = LIGBASE_NOT_CHECKED;

  for (unsigned i = 1; i < count; i++) {
    if (!it.next()) return false;
    c->matchPositions[i] = static_cast<uint16_t>(it.idx);
    const GlyphInfo& info = buffer.info[it.idx];
    const unsigned thisLigId = info.ligId();
    const unsigned thisLigComp = info.ligComp();
    if (firstLigId && firstLigComp) {
      if (firstLigId != thisLigId || firstLigComp != thisLigComp) {
        if (ligbase == LIGBASE_NOT_CHECKED) {
          bool found = false;
          unsigned j = buffer.idx;  // out_len
          while (j && buffer.info[j - 1].ligId() == firstLigId) {
            if (buffer.info[j - 1].ligComp() == 0) {
              j--;
              found = true;
              break;
            }
            j--;
          }
          ligbase =
              found && it.maySkip(buffer.info[j]) == SkippyIter::SKIP_YES ? LIGBASE_MAY_SKIP : LIGBASE_MAY_NOT_SKIP;
        }
        if (ligbase == LIGBASE_MAY_NOT_SKIP) return false;
      }
    } else if (thisLigId && thisLigComp && thisLigId != firstLigId) {
      return false;
    }
    total += info.ligNumCompsInLigation();
  }
  *endPosition = it.idx + 1;
  if (totalComponentCount) *totalComponentCount = total + buffer.cur().ligNumComps();
  c->matchPositions[0] = static_cast<uint16_t>(buffer.idx);
  return true;
}

// ligate_input(), editing the glyph array in place: the ligature replaces the
// first component, glyphs skipped between components stay, the other
// components are removed.
void ligateInput(ApplyContext* c, const unsigned count, const uint32_t ligGlyph, const unsigned totalComponentCount) {
  Buffer& buffer = c->buffer;
  const uint16_t* mp = c->matchPositions;

  bool isBaseLigature = buffer.info[mp[0]].isBaseGlyph();
  bool isMarkLigature = buffer.info[mp[0]].isMark();
  for (unsigned i = 1; i < count; i++) {
    if (!buffer.info[mp[i]].isMark()) {
      isBaseLigature = false;
      isMarkLigature = false;
      break;
    }
  }
  const bool isLigature = !isBaseLigature && !isMarkLigature;
  const uint16_t klass = isLigature ? props::LIGATURE : 0;
  const unsigned ligId = isLigature ? buffer.allocateLigId() : 0;
  unsigned lastLigId = buffer.cur().ligId();
  unsigned lastNumComponents = buffer.cur().ligNumComps();
  unsigned componentsSoFar = lastNumComponents;

  if (isLigature) {
    GlyphInfo& cur = buffer.cur();
    cur.setLigPropsForLigature(ligId, totalComponentCount);
    if (cur.genCat() == gc::NON_SPACING_MARK) {
      cur.unicodeProps = static_cast<uint16_t>(gc::OTHER_LETTER | (cur.unicodeProps & (0xFF & ~uprops::GEN_CAT)));
    }
  }
  {
    GlyphInfo& cur = buffer.cur();
    c->setGlyphClass(cur, ligGlyph, klass, true);
    cur.codepoint = ligGlyph;
    buffer.idx++;
  }

  unsigned removed = 0;
  for (unsigned i = 1; i < count; i++) {
    const unsigned target = mp[i] - removed;
    while (buffer.idx < target) {
      if (isLigature) {
        GlyphInfo& cur = buffer.cur();
        unsigned thisComp = cur.ligComp();
        if (thisComp == 0) thisComp = lastNumComponents;
        const unsigned newLigComp =
            componentsSoFar - lastNumComponents + (thisComp < lastNumComponents ? thisComp : lastNumComponents);
        cur.setLigPropsForMark(ligId, newLigComp);
      }
      buffer.idx++;
    }
    lastLigId = buffer.cur().ligId();
    lastNumComponents = buffer.cur().ligNumCompsInLigation();
    componentsSoFar += lastNumComponents;
    buffer.removeGlyph(buffer.idx);  // the component joins the ligature
    removed++;
  }

  if (!isMarkLigature && lastLigId) {
    for (unsigned i = buffer.idx; i < buffer.len(); ++i) {
      GlyphInfo& info = buffer.info[i];
      if (lastLigId != info.ligId()) break;
      const unsigned thisComp = info.ligComp();
      if (!thisComp) break;
      const unsigned newLigComp =
          componentsSoFar - lastNumComponents + (thisComp < lastNumComponents ? thisComp : lastNumComponents);
      info.setLigPropsForMark(ligId, newLigComp);
    }
  }
}

bool matchBacktrack(ApplyContext* c, const unsigned count, const Values& values, const MatchFunc func,
                    const Table& data) {
  if (!count) return true;
  SkippyIter& it = c->iterContext;
  it.reset(c->buffer.idx);  // reset_back: backtrack_len() == idx here
  it.setMatch(func, data, values);
  for (unsigned i = 0; i < count; i++) {
    if (!it.prev()) return false;
  }
  return true;
}

bool matchLookahead(ApplyContext* c, const unsigned count, const Values& values, const MatchFunc func,
                    const Table& data, const unsigned startIndex) {
  if (!count) return true;
  SkippyIter& it = c->iterContext;
  it.reset(startIndex - 1);
  it.setMatch(func, data, values);
  for (unsigned i = 0; i < count; i++) {
    if (!it.next()) return false;
  }
  return true;
}

namespace {

// apply_lookup(): runs the rule's nested lookups at their sequence
// positions, shifting later positions as nested lookups grow or shrink the
// glyph string.
void applyNestedLookups(ApplyContext* c, unsigned count, const unsigned lookupCount, const Values& records,
                        const unsigned matchEnd) {
  Buffer& buffer = c->buffer;
  uint16_t* mp = c->matchPositions;
  int end = static_cast<int>(matchEnd);

  for (unsigned i = 0; i < lookupCount && buffer.successful; i++) {
    const unsigned idx = records.t.u16(records.offset + 4 * i);
    const uint16_t lookupListIndex = records.t.u16(records.offset + 4 * i + 2);
    if (idx >= count) continue;
    const unsigned origLen = buffer.len();
    if (mp[idx] >= origLen) continue;
    buffer.idx = mp[idx];
    if (buffer.maxOps <= 0) break;
    if (!c->recurse(lookupListIndex)) continue;

    const unsigned newLen = buffer.len();
    int delta = static_cast<int>(newLen) - static_cast<int>(origLen);
    if (!delta) continue;
    end += delta;
    if (end < static_cast<int>(mp[idx])) {
      delta += static_cast<int>(mp[idx]) - end;
      end = static_cast<int>(mp[idx]);
    }
    unsigned next = idx + 1;
    if (delta > 0) {
      if (delta + count > MAX_CONTEXT_LENGTH) break;
    } else {
      const int floor = static_cast<int>(next) - static_cast<int>(count);
      if (delta < floor) delta = floor;
      next = static_cast<unsigned>(static_cast<int>(next) - delta);
    }
    memmove(mp + next + delta, mp + next, (count - next) * sizeof(mp[0]));
    next = static_cast<unsigned>(static_cast<int>(next) + delta);
    count = static_cast<unsigned>(static_cast<int>(count) + delta);
    for (unsigned j = idx + 1; j < next; j++) mp[j] = static_cast<uint16_t>(mp[j - 1] + 1);
    for (; next < count; next++) mp[next] = static_cast<uint16_t>(static_cast<int>(mp[next]) + delta);
  }
  buffer.idx = static_cast<unsigned>(end < 0 ? 0 : end);
}

bool contextApplyLookup(ApplyContext* c, const unsigned inputCount, const Values& input, const unsigned lookupCount,
                        const Values& records, const MatchFunc func, const Table& data) {
  if (inputCount > MAX_CONTEXT_LENGTH) return false;
  unsigned matchEnd = 0;
  if (!matchInput(c, inputCount, input, func, data, &matchEnd)) return false;
  applyNestedLookups(c, inputCount, lookupCount, records, matchEnd);
  return true;
}

struct ChainFuncs {
  MatchFunc func[3];  // backtrack, input, lookahead
  Table data[3];
};

bool chainContextApplyLookup(ApplyContext* c, const unsigned backtrackCount, const Values& backtrack,
                             const unsigned inputCount, const Values& input, const unsigned lookaheadCount,
                             const Values& lookahead, const unsigned lookupCount, const Values& records,
                             const ChainFuncs& f) {
  if (inputCount > MAX_CONTEXT_LENGTH) return false;
  unsigned matchEnd = 0;
  if (!matchInput(c, inputCount, input, f.func[1], f.data[1], &matchEnd)) return false;
  if (!matchLookahead(c, lookaheadCount, lookahead, f.func[2], f.data[2], matchEnd)) return false;
  if (!matchBacktrack(c, backtrackCount, backtrack, f.func[0], f.data[0])) return false;
  applyNestedLookups(c, inputCount, lookupCount, records, matchEnd);
  return true;
}

// Rule and ChainRule layouts, shared by formats 1 and 2 (glyphs or classes).
bool applyRule(ApplyContext* c, const Table& rule, const MatchFunc func, const Table& data) {
  const uint16_t inputCount = rule.u16(0);
  const uint16_t lookupCount = rule.u16(2);
  const uint32_t records = 4 + 2 * (inputCount ? inputCount - 1u : 0u);
  return contextApplyLookup(c, inputCount, Values{rule, 4}, lookupCount, Values{rule, records}, func, data);
}

bool applyChainRule(ApplyContext* c, const Table& rule, const ChainFuncs& f) {
  uint32_t o = 0;
  const uint16_t backtrackCount = rule.u16(o);
  const Values& backtrack{rule, o + 2};
  o += 2 + 2u * backtrackCount;
  const uint16_t inputCount = rule.u16(o);
  const Values& input{rule, o + 2};
  o += 2 + 2u * (inputCount ? inputCount - 1u : 0u);
  const uint16_t lookaheadCount = rule.u16(o);
  const Values& lookahead{rule, o + 2};
  o += 2 + 2u * lookaheadCount;
  const uint16_t lookupCount = rule.u16(o);
  const Values& records{rule, o + 2};
  return chainContextApplyLookup(c, backtrackCount, backtrack, inputCount, input, lookaheadCount, lookahead,
                                 lookupCount, records, f);
}

bool applyRuleSet(ApplyContext* c, const Table& ruleSet, const MatchFunc func, const Table& data) {
  const uint16_t count = ruleSet.u16(0);
  for (uint16_t i = 0; i < count; i++) {
    if (applyRule(c, ruleSet.offset16(2 + 2 * i), func, data)) return true;
  }
  return false;
}

bool applyChainRuleSet(ApplyContext* c, const Table& ruleSet, const ChainFuncs& f) {
  const uint16_t count = ruleSet.u16(0);
  for (uint16_t i = 0; i < count; i++) {
    if (applyChainRule(c, ruleSet.offset16(2 + 2 * i), f)) return true;
  }
  return false;
}

}  // namespace

bool applyContext(ApplyContext* c, const Table& st) {
  const uint32_t glyph = c->buffer.cur().codepoint;
  switch (st.u16(0)) {
    case 1: {
      const int index = coverageIndex(st.offset16(2), glyph);
      if (index == NOT_COVERED || static_cast<unsigned>(index) >= st.u16(4)) return false;
      return applyRuleSet(c, st.offset16(6 + 2 * index), MatchFunc::Glyph, Table());
    }
    case 2: {
      if (coverageIndex(st.offset16(2), glyph) == NOT_COVERED) return false;
      const Table classDef = st.offset16(4);
      const uint16_t klass = classOf(classDef, glyph);
      if (klass >= st.u16(6)) return false;
      return applyRuleSet(c, st.offset16(8 + 2 * klass), MatchFunc::Class, classDef);
    }
    case 3: {
      const uint16_t glyphCount = st.u16(2);
      const uint16_t lookupCount = st.u16(4);
      if (coverageIndex(st.offset16(6), glyph) == NOT_COVERED) return false;
      return contextApplyLookup(c, glyphCount, Values{st, 8}, lookupCount, Values{st, 6 + 2u * glyphCount},
                                MatchFunc::Coverage, st);
    }
    default:
      return false;
  }
}

bool applyChainContext(ApplyContext* c, const Table& st) {
  const uint32_t glyph = c->buffer.cur().codepoint;
  switch (st.u16(0)) {
    case 1: {
      const int index = coverageIndex(st.offset16(2), glyph);
      if (index == NOT_COVERED || static_cast<unsigned>(index) >= st.u16(4)) return false;
      const ChainFuncs f{{MatchFunc::Glyph, MatchFunc::Glyph, MatchFunc::Glyph}, {}};
      return applyChainRuleSet(c, st.offset16(6 + 2 * index), f);
    }
    case 2: {
      if (coverageIndex(st.offset16(2), glyph) == NOT_COVERED) return false;
      const ChainFuncs f{{MatchFunc::Class, MatchFunc::Class, MatchFunc::Class},
                         {st.offset16(4), st.offset16(6), st.offset16(8)}};
      const uint16_t klass = classOf(f.data[1], glyph);
      if (klass >= st.u16(10)) return false;
      return applyChainRuleSet(c, st.offset16(12 + 2 * klass), f);
    }
    case 3: {
      uint32_t o = 2;
      const uint16_t backtrackCount = st.u16(o);
      const Values& backtrack{st, o + 2};
      o += 2 + 2u * backtrackCount;
      const uint16_t inputCount = st.u16(o);
      const uint32_t inputArray = o + 2;
      o += 2 + 2u * inputCount;
      const uint16_t lookaheadCount = st.u16(o);
      const Values& lookahead{st, o + 2};
      o += 2 + 2u * lookaheadCount;
      const uint16_t lookupCount = st.u16(o);
      const Values& records{st, o + 2};
      if (coverageIndex(st.offset16(inputArray), glyph) == NOT_COVERED) return false;
      const ChainFuncs f{{MatchFunc::Coverage, MatchFunc::Coverage, MatchFunc::Coverage}, {st, st, st}};
      return chainContextApplyLookup(c, backtrackCount, backtrack, inputCount, Values{st, inputArray + 2},
                                     lookaheadCount, lookahead, lookupCount, records, f);
    }
    default:
      return false;
  }
}

}  // namespace ot::layout

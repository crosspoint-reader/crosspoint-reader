#include "OtLayout.h"

#include "OtLayoutInternal.h"

// Lookup application: the skipping iterator, the per-lookup apply context,
// subtable dispatch and nested-lookup recursion (hb-ot-layout-gsubgpos.hh,
// hb-ot-layout.cc apply_forward/apply_backward). The subtable formats live in
// OtContext.cpp, OtGsub.cpp and OtGpos.cpp.

namespace ot {

namespace layout {

void resolveExtension(const int table, uint16_t& type, Table& subtable) {
  const uint16_t extension = table == GSUB ? gsub::EXTENSION : gpos::EXTENSION;
  if (type != extension) return;
  if (subtable.u16(0) != 1) {
    subtable = Table();
    return;
  }
  type = subtable.u16(2);
  subtable = subtable.offset32(4);
  if (type == extension) subtable = Table();
}

namespace {

// The coverage table a subtable starts matching with (get_coverage()).
Table subtableCoverage(const int table, uint16_t type, Table st) {
  resolveExtension(table, type, st);
  const bool isContext = (table == GSUB && type == gsub::CONTEXT) || (table == GPOS && type == gpos::CONTEXT);
  const bool isChain = (table == GSUB && type == gsub::CHAIN) || (table == GPOS && type == gpos::CHAIN);
  if (isContext) return st.u16(0) == 3 ? st.offset16(6) : st.offset16(2);
  if (isChain) {
    if (st.u16(0) == 3) return st.offset16(4 + 2 * st.u16(2) + 2);  // first input coverage, after the backtrack
    return st.offset16(2);
  }
  if (st.empty()) return Table();
  return st.offset16(2);
}

void collectCoverage(const Table& coverage, Digest& digest) {
  switch (coverage.u16(0)) {
    case 1:
      for (uint16_t i = 0; i < coverage.count16(2, 4, 2); i++) digest.add(coverage.u16(4 + 2 * i));
      break;
    case 2:
      for (uint16_t i = 0; i < coverage.count16(2, 4, 6); i++) {
        digest.addRange(coverage.u16(4 + 6 * i), coverage.u16(6 + 6 * i));
      }
      break;
    default:
      break;
  }
}

bool isReverseLookup(const Table& lookup) {
  uint16_t type = lookup.u16(0);
  if (type == gsub::EXTENSION) {
    Table st = lookup.offset16(6);
    resolveExtension(GSUB, type, st);
  }
  return type == gsub::REVERSE_CHAIN;
}

}  // namespace

// --- Skipping iterator ----------------------------------------------------------

void SkippyIter::init(ApplyContext* ctx, const bool contextMatch) {
  c = ctx;
  end = c->buffer.len();
  hasValues = false;
  func = MatchFunc::None;
  lookupProps = c->lookupProps;
  ignoreZwnj = c->table == GPOS || (contextMatch && c->autoZwnj);
  ignoreZwj = contextMatch || c->autoZwj;
  ignoreHidden = c->table == GPOS;
  mask = contextMatch ? ~0u : c->lookupMask;
  perSyllable = c->table == GSUB && c->perSyllable;
  syllable = 0;
}

void SkippyIter::reset(const unsigned start) {
  idx = start;
  end = c->buffer.len();
  syllable = c->buffer.idx < c->buffer.len() ? c->buffer.cur().syllable : 0;
}

SkippyIter::MaySkip SkippyIter::maySkip(const GlyphInfo& info) const {
  if (!c->checkGlyphProperty(info, lookupProps)) return SKIP_YES;
  if (info.isDefaultIgnorable() && (ignoreZwnj || !info.isZwnj()) && (ignoreZwj || !info.isZwj()) &&
      (ignoreHidden || !info.isHidden())) {
    return SKIP_MAYBE;
  }
  return SKIP_NO;
}

SkippyIter::MayMatch SkippyIter::mayMatch(const GlyphInfo& info) const {
  if (!(info.mask & mask) || (perSyllable && syllable && syllable != info.syllable)) return MATCH_NO;
  const uint32_t value = hasValues ? values.t.u16(values.offset) : 0;
  switch (func) {
    case MatchFunc::None:
      return MATCH_MAYBE;
    case MatchFunc::Glyph:
      return info.codepoint == value ? MATCH_YES : MATCH_NO;
    case MatchFunc::Class:
      return classOf(matchData, info.codepoint) == value ? MATCH_YES : MATCH_NO;
    case MatchFunc::Coverage:
      return coverageIndex(matchData.at(value), info.codepoint) != NOT_COVERED ? MATCH_YES : MATCH_NO;
    case MatchFunc::Always:
      return MATCH_YES;
  }
  return MATCH_NO;
}

SkippyIter::Result SkippyIter::match(const GlyphInfo& info) const {
  const MaySkip skip = maySkip(info);
  if (skip == SKIP_YES) return SKIP;
  const MayMatch m = mayMatch(info);
  if (m == MATCH_YES || (m == MATCH_MAYBE && skip == SKIP_NO)) return MATCH;
  if (skip == SKIP_NO) return NOT_MATCH;
  return SKIP;
}

bool SkippyIter::next() {
  const auto& info = c->buffer.info;
  while (static_cast<int>(idx) < static_cast<int>(end) - 1) {
    idx++;
    switch (match(info[idx])) {
      case MATCH:
        if (hasValues) values.offset += 2;
        return true;
      case NOT_MATCH:
        return false;
      case SKIP:
        continue;
    }
  }
  return false;
}

bool SkippyIter::prev() {
  const auto& info = c->buffer.info;
  while (idx > 0) {
    idx--;
    switch (match(info[idx])) {
      case MATCH:
        if (hasValues) values.offset += 2;
        return true;
      case NOT_MATCH:
        return false;
      case SKIP:
        continue;
    }
  }
  return false;
}

// --- Apply context --------------------------------------------------------------

bool ApplyContext::checkGlyphProperty(const GlyphInfo& info, const uint32_t matchProps) const {
  const uint16_t glyphProps = info.glyphProps;
  if (glyphProps & matchProps & lookupflag::IGNORE_FLAGS) return false;
  if (!(glyphProps & props::MARK)) return true;
  // match_properties_mark(): mark filtering set, else mark attachment type.
  if (matchProps & lookupflag::USE_MARK_FILTERING_SET) return face.markSetCovers(matchProps >> 16, info.codepoint);
  if (matchProps & lookupflag::MARK_ATTACHMENT_TYPE) {
    return (matchProps & lookupflag::MARK_ATTACHMENT_TYPE) == (glyphProps & lookupflag::MARK_ATTACHMENT_TYPE);
  }
  return true;
}

void ApplyContext::setGlyphClass(GlyphInfo& info, const uint32_t glyph, const uint16_t classGuess, const bool ligature,
                                 const bool component) const {
  buffer.digest.add(glyph);
  uint16_t p = info.glyphProps | props::SUBSTITUTED;
  if (ligature) {
    // Only the last of ligation and multiplication counts.
    p |= props::LIGATED;
    p &= ~props::MULTIPLIED;
  }
  if (component) p |= props::MULTIPLIED;
  if (hasGlyphClasses) {
    info.glyphProps = static_cast<uint16_t>((p & props::PRESERVE) | face.glyphProps(glyph));
  } else if (classGuess) {
    info.glyphProps = static_cast<uint16_t>((p & props::PRESERVE) | classGuess);
  } else {
    info.glyphProps = p;
  }
}

void ApplyContext::replaceGlyph(const uint32_t glyph) {
  replaceGlyphInplace(glyph);
  buffer.idx++;
}

void ApplyContext::replaceGlyphInplace(const uint32_t glyph) {
  GlyphInfo& cur = buffer.cur();
  setGlyphClass(cur, glyph);
  cur.codepoint = glyph;
}

bool ApplyContext::applySubtable(uint16_t type, Table st) {
  resolveExtension(table, type, st);
  if (st.empty()) return false;
  return table == GSUB ? applyGsubSubtable(this, type, st) : applyGposSubtable(this, type, st);
}

bool ApplyContext::applyLookupSubtables(const Table& lookup, const uint8_t* digests) {
  const uint16_t type = lookup.u16(0);
  const uint16_t count = Face::subtableCount(lookup);
  const uint32_t glyph = buffer.cur().codepoint;
  for (uint16_t i = 0; i < count; i++) {
    if (digests && !digestRecordMayHave(digests + DIGEST_BYTES * i, glyph)) continue;
    if (applySubtable(type, lookup.offset16(6 + 2 * i))) return true;
  }
  return false;
}

// dispatch_recurse_func(): a nested lookup applies once at buffer.idx, with
// its own flags and its own row of match positions, and without the mask
// and glyph-property check the top-level loop makes.
bool ApplyContext::recurse(const uint32_t subLookupIndex) {
  if (nestingLeft == 0 || --buffer.maxOps < 0) {
    buffer.successful = false;
    return false;
  }
  const Table lookup = face.lookup(table, subLookupIndex);
  if (lookup.empty()) return false;
  const uint32_t savedProps = lookupProps;
  uint16_t* savedPositions = matchPositions;
  nestingLeft--;
  matchPositions = buffer.matchStack[MAX_NESTING_LEVEL - nestingLeft];
  setLookupProps(face.lookupProps(lookup));
  const bool ret = applyLookupSubtables(lookup);
  setLookupProps(savedProps);
  matchPositions = savedPositions;
  nestingLeft++;
  return ret;
}

}  // namespace layout

using layout::ApplyContext;

Digest lookupDigest(const Face& face, const int table, const uint32_t lookupIndex) {
  Digest digest;
  const Table lookup = face.lookup(table, lookupIndex);
  for (uint16_t i = 0; i < Face::subtableCount(lookup); i++) {
    layout::collectCoverage(layout::subtableCoverage(table, lookup.u16(0), lookup.offset16(6 + 2 * i)), digest);
  }
  return digest;
}

void appendSubtableDigests(const Face& face, const int table, const uint32_t lookupIndex, std::vector<uint8_t>& out) {
  const Table lookup = face.lookup(table, lookupIndex);
  for (uint16_t i = 0; i < Face::subtableCount(lookup); i++) {
    Digest d;
    layout::collectCoverage(layout::subtableCoverage(table, lookup.u16(0), lookup.offset16(6 + 2 * i)), d);
    out.resize(out.size() + DIGEST_BYTES);
    writeDigest(d, out.data() + out.size() - DIGEST_BYTES);
  }
}

void applyLookup(const Face& face, const Scale& scale, Buffer& buffer, const int table, const uint32_t lookupIndex,
                 const LookupSettings& settings) {
  if (!buffer.len() || !settings.mask) return;
  const Table lookup = face.lookup(table, lookupIndex);
  if (lookup.empty()) return;

  ApplyContext c(face, scale, buffer, table);
  c.lookupMask = settings.mask;
  c.autoZwj = settings.autoZwj;
  c.autoZwnj = settings.autoZwnj;
  c.random = settings.random;
  c.perSyllable = settings.perSyllable;
  c.subtableDigests = settings.subtableDigests;
  c.setLookupProps(face.lookupProps(lookup));

  // Glyphs the lookup may start at: in its digest, selected by its mask, and
  // not ignored by its flags.
  const auto startsAt = [&](const GlyphInfo& g) {
    return (!settings.hasDigest || settings.digest.mayHave(g.codepoint)) && (g.mask & c.lookupMask) &&
           c.checkGlyphProperty(g, c.lookupProps);
  };

  if (table == GSUB && layout::isReverseLookup(lookup)) {
    // In place, from the end; the loop, not the lookup, moves idx.
    for (buffer.idx = buffer.len(); buffer.idx-- > 0;) {
      if (startsAt(buffer.cur())) c.applyLookupSubtables(lookup, c.subtableDigests);
    }
    return;
  }

  buffer.idx = 0;
  while (buffer.successful) {
    while (buffer.idx < buffer.len() && !startsAt(buffer.info[buffer.idx])) buffer.idx++;
    if (buffer.idx >= buffer.len()) break;
    const unsigned idx = buffer.idx, len = buffer.len();
    // An applied subtable moves past its glyphs; one that leaves the buffer
    // as it was would apply again forever.
    if (!c.applyLookupSubtables(lookup, c.subtableDigests) || (buffer.idx == idx && buffer.len() == len)) {
      buffer.idx++;
    }
  }
}

bool wouldSubstitute(const Face& face, const uint32_t lookupIndex, const Digest& digest, const uint32_t* glyphs,
                     const unsigned count, const bool zeroContext) {
  if (!count || lookupIndex >= face.lookupCount(GSUB)) return false;
  if (!digest.mayHave(glyphs[0])) return false;
  const Table lookup = face.lookup(GSUB, lookupIndex);
  for (uint16_t i = 0; i < Face::subtableCount(lookup); i++) {
    if (layout::wouldApplyGsubSubtable(glyphs, count, zeroContext, lookup.u16(0), lookup.offset16(6 + 2 * i))) {
      return true;
    }
  }
  return false;
}

}  // namespace ot

#pragma once

// Shared by the lookup-application files (OtLayout.cpp, OtContext.cpp,
// OtGsub.cpp, OtGpos.cpp). Not part of the public API.
//
// The matching rules (which glyphs a lookup skips, how ligatures track their
// components, how marks find their base, how nested lookups shift match
// positions) follow HarfBuzz's hb-ot-layout-gsubgpos.hh and
// OT/Layout/{GSUB,GPOS} closely, so shaping output is identical; comments
// name the HarfBuzz function each part mirrors.

#include <cstdint>

#include "OtBuffer.h"
#include "OtFace.h"
#include "OtLayout.h"

namespace ot::layout {

// Lookup types.
namespace gsub {
constexpr uint16_t SINGLE = 1, MULTIPLE = 2, ALTERNATE = 3, LIGATURE = 4, CONTEXT = 5, CHAIN = 6, EXTENSION = 7,
                   REVERSE_CHAIN = 8;
}
namespace gpos {
constexpr uint16_t SINGLE = 1, PAIR = 2, CURSIVE = 3, MARK_BASE = 4, MARK_LIG = 5, MARK_MARK = 6, CONTEXT = 7,
                   CHAIN = 8, EXTENSION = 9;
}

// Links followed through chains of attached glyphs (HarfBuzz uses its
// nesting limit, 64, for these too).
constexpr unsigned MAX_ATTACH_DEPTH = 64;

inline int32_t saturate(const int64_t v) {
  if (v > INT32_MAX) return INT32_MAX;
  if (v < INT32_MIN) return INT32_MIN;
  return static_cast<int32_t>(v);
}
inline int32_t satAdd(const int32_t a, const int32_t b) { return saturate(static_cast<int64_t>(a) + b); }
inline int32_t satSub(const int32_t a, const int32_t b) { return saturate(static_cast<int64_t>(a) - b); }

// Resolves an Extension subtable to the one it wraps (type and table).
void resolveExtension(int table, uint16_t& type, Table& subtable);

// What a context rule compares each glyph with.
enum class MatchFunc : uint8_t { None, Glyph, Class, Coverage, Always };

// A u16 array of values (glyph IDs, classes or coverage offsets) in `t`
// starting at `offset`.
struct Values {
  Table t;
  uint32_t offset;
};

struct ApplyContext;

// matcher_t + skipping_iterator_t: walks the buffer from a position,
// skipping glyphs the lookup ignores.
struct SkippyIter {
  enum Result { MATCH, NOT_MATCH, SKIP };
  enum MaySkip { SKIP_NO, SKIP_YES, SKIP_MAYBE };
  enum MayMatch { MATCH_NO, MATCH_YES, MATCH_MAYBE };

  ApplyContext* c = nullptr;
  unsigned idx = 0;
  unsigned end = 0;

  uint32_t lookupProps = 0;
  uint32_t mask = ~0u;
  bool ignoreZwnj = false;
  bool ignoreZwj = false;
  bool ignoreHidden = false;
  bool perSyllable = false;
  uint8_t syllable = 0;
  MatchFunc func = MatchFunc::None;
  Table matchData;  // ClassDef (Class) or the table coverage offsets are relative to (Coverage)
  Values values{};
  bool hasValues = false;

  void init(ApplyContext* ctx, bool contextMatch);
  void setMatch(MatchFunc f, const Table& data, const Values& v) {
    func = f;
    matchData = data;
    values = v;
    hasValues = true;
  }
  void reset(unsigned start);
  void resetFast(const unsigned start) { idx = start; }

  MaySkip maySkip(const GlyphInfo& info) const;
  MayMatch mayMatch(const GlyphInfo& info) const;
  Result match(const GlyphInfo& info) const;
  bool next();
  bool prev();
};

// hb_ot_apply_context_t: one lookup being applied to the buffer.
struct ApplyContext {
  const Face& face;
  const Scale& scale;
  Buffer& buffer;
  const int table;  // GSUB or GPOS
  const bool hasGlyphClasses;

  uint32_t lookupMask = 1;
  uint32_t lookupProps = 0;
  unsigned nestingLeft = MAX_NESTING_LEVEL;
  bool autoZwnj = true;
  bool autoZwj = true;
  bool perSyllable = false;
  bool random = false;

  // Mark attachment's memo of the last base found (last_base, last_base_until).
  int lastBase = -1;
  unsigned lastBaseUntil = 0;

  // Subtable digests of the top-level lookup (not of nested ones).
  const uint8_t* subtableDigests = nullptr;

  // Match positions of the rule being applied: this nesting level's row of
  // buffer.matchStack, so nested lookups keep their own.
  uint16_t* matchPositions;

  SkippyIter iterInput;    // matches a rule's input glyphs
  SkippyIter iterContext;  // matches backtrack and lookahead

  ApplyContext(const Face& f, const Scale& s, Buffer& b, const int t)
      : face(f), scale(s), buffer(b), table(t), hasGlyphClasses(f.hasGlyphClasses()), matchPositions(b.matchStack[0]) {}

  void initIters() {
    iterInput.init(this, false);
    iterContext.init(this, true);
  }
  void setLookupProps(const uint32_t props) {
    lookupProps = face.sanitizeLookupProps(props);
    initIters();
  }

  uint32_t randomNumber() {
    buffer.randomState = static_cast<uint32_t>(static_cast<uint64_t>(buffer.randomState) * 48271 % 2147483647);
    return buffer.randomState;
  }

  bool checkGlyphProperty(const GlyphInfo& info, uint32_t matchProps) const;
  // _set_glyph_class().
  void setGlyphClass(GlyphInfo& info, uint32_t glyph, uint16_t classGuess = 0, bool ligature = false,
                     bool component = false) const;
  void replaceGlyph(uint32_t glyph);
  void replaceGlyphInplace(uint32_t glyph);

  bool applySubtable(uint16_t type, Table st);
  // Tries each subtable at buffer.idx; `digests` (optional) skips the ones
  // that cannot start there.
  bool applyLookupSubtables(const Table& lookup, const uint8_t* digests = nullptr);
  bool recurse(uint32_t subLookupIndex);
};

// Context matching (OtContext.cpp).
bool matchInput(ApplyContext* c, unsigned count, const Values& input, MatchFunc func, const Table& data,
                unsigned* endPosition, unsigned* totalComponentCount = nullptr);
void ligateInput(ApplyContext* c, unsigned count, uint32_t ligGlyph, unsigned totalComponentCount);
bool matchBacktrack(ApplyContext* c, unsigned count, const Values& values, MatchFunc func, const Table& data);
bool matchLookahead(ApplyContext* c, unsigned count, const Values& values, MatchFunc func, const Table& data,
                    unsigned startIndex);
bool applyContext(ApplyContext* c, const Table& st);
bool applyChainContext(ApplyContext* c, const Table& st);

// Subtable application (OtGsub.cpp, OtGpos.cpp).
bool applyGsubSubtable(ApplyContext* c, uint16_t type, const Table& st);
bool applyGposSubtable(ApplyContext* c, uint16_t type, const Table& st);
bool wouldApplyGsubSubtable(const uint32_t* glyphs, unsigned count, bool zeroContext, uint16_t type, Table st);

}  // namespace ot::layout

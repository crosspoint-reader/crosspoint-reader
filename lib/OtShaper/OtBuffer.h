#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "OtFace.h"

namespace ot {

// Unicode properties per glyph (HarfBuzz's unicode_props): general category
// in the low five bits, flags above, and for marks the combining class (for
// ZWJ/ZWNJ the joiner flags) in the high byte.
namespace uprops {
constexpr uint16_t GEN_CAT = 0x001F;
constexpr uint16_t IGNORABLE = 0x0020;
constexpr uint16_t HIDDEN = 0x0040;
constexpr uint16_t CONTINUATION = 0x0080;
constexpr uint16_t CF_ZWJ = 0x0100;
constexpr uint16_t CF_ZWNJ = 0x0200;
}  // namespace uprops

// hb_unicode_general_category_t values used here.
namespace gc {
constexpr uint8_t FORMAT = 1;
constexpr uint8_t UNASSIGNED = 2;
constexpr uint8_t OTHER_LETTER = 7;
constexpr uint8_t SPACING_MARK = 10;
constexpr uint8_t NON_SPACING_MARK = 12;
constexpr bool isMark(const uint8_t cat) { return cat >= SPACING_MARK && cat <= NON_SPACING_MARK; }
}  // namespace gc

// Asked before the shaper takes heap memory that depends on the font or the
// text (plan filters, buffer growth): whether `bytes` more may be allocated.
// The default allows any size. Global; set it once, before shaping.
using HeapCheck = bool (*)(size_t bytes);
void setHeapCheck(HeapCheck check);
bool heapAvailable(size_t bytes);

// HarfBuzz's set digest: a three-mask Bloom filter over glyph IDs. Whether a
// lookup "would substitute" depends on it for some subtable formats, so it is
// reproduced exactly; it also lets lookup application skip glyphs and lookups
// that cannot match.
struct Digest {
  uint64_t masks[3] = {0, 0, 0};

  void add(uint32_t g);
  void addRange(uint32_t a, uint32_t b);
  bool mayHave(uint32_t g) const;
  bool mayIntersect(const Digest& o) const {
    return (masks[0] & o.masks[0]) && (masks[1] & o.masks[1]) && (masks[2] & o.masks[2]);
  }
};

// Digests as stored in memory and in the CPac table: the three masks as
// little-endian 64-bit words. Records need not be aligned.
constexpr unsigned DIGEST_BYTES = 24;
Digest readDigest(const uint8_t* record);
void writeDigest(const Digest& digest, uint8_t* record);
// Whether the digest record may hold `glyph`, reading only as many masks as
// it takes to rule the glyph out.
bool digestRecordMayHave(const uint8_t* record, uint32_t glyph);

struct GlyphInfo {
  uint32_t codepoint;   // Unicode until glyphs are mapped, then the glyph ID
  uint32_t mask;        // feature bits that apply to this glyph
  uint16_t glyphIndex;  // the normalizer's glyph for `codepoint`
  uint16_t glyphProps;
  uint16_t unicodeProps;
  uint8_t ligProps;  // lig_id:3 | IS_LIG_BASE | component:4
  uint8_t syllable;  // serial:4 | syllable type:4
  uint8_t category;  // shaper's character category
  uint8_t position;  // Indic shaper's visual position

  uint8_t genCat() const { return unicodeProps & uprops::GEN_CAT; }
  bool isUnicodeMark() const { return gc::isMark(genCat()); }
  uint8_t combiningClass() const { return isUnicodeMark() ? unicodeProps >> 8 : 0; }
  bool isDefaultIgnorable() const { return (unicodeProps & uprops::IGNORABLE) && !(glyphProps & props::SUBSTITUTED); }
  bool isZwnj() const { return genCat() == gc::FORMAT && (unicodeProps & uprops::CF_ZWNJ); }
  bool isZwj() const { return genCat() == gc::FORMAT && (unicodeProps & uprops::CF_ZWJ); }
  bool isHidden() const { return unicodeProps & uprops::HIDDEN; }

  uint8_t syllableType() const { return syllable & 0x0F; }
  static uint8_t packSyllable(const unsigned serial, const unsigned type) {
    return static_cast<uint8_t>((serial << 4) | type);
  }

  bool isBaseGlyph() const { return glyphProps & props::BASE_GLYPH; }
  bool isLigature() const { return glyphProps & props::LIGATURE; }
  bool isMark() const { return glyphProps & props::MARK; }
  bool substituted() const { return glyphProps & props::SUBSTITUTED; }
  bool ligated() const { return glyphProps & props::LIGATED; }
  bool multiplied() const { return glyphProps & props::MULTIPLIED; }
  bool ligatedAndDidntMultiply() const { return ligated() && !multiplied(); }

  static constexpr uint8_t IS_LIG_BASE = 0x10;
  unsigned ligId() const { return ligProps >> 5; }
  bool ligatedInternal() const { return ligProps & IS_LIG_BASE; }
  unsigned ligComp() const { return ligatedInternal() ? 0 : ligProps & 0x0F; }
  unsigned ligNumComps() const { return (isLigature() && ligatedInternal()) ? ligProps & 0x0F : 1; }
  // Components this glyph contributes when ligated again: the later pieces
  // of a MultipleSubst belong to the first piece's component.
  unsigned ligNumCompsInLigation() const { return (multiplied() && ligComp()) ? 0 : ligNumComps(); }
  void setLigPropsForLigature(const unsigned id, const unsigned comps) {
    ligProps = static_cast<uint8_t>((id << 5) | IS_LIG_BASE | (comps & 0x0F));
  }
  void setLigPropsForMark(const unsigned id, const unsigned comp) {
    ligProps = static_cast<uint8_t>((id << 5) | (comp & 0x0F));
  }
};

struct GlyphPosition {
  int32_t xAdvance;
  int32_t yAdvance;
  int32_t xOffset;
  int32_t yOffset;
  int16_t attachChain;  // relative index of the glyph this one attaches to
  uint8_t attachType;   // ATTACH_TYPE_*
};

constexpr uint8_t ATTACH_TYPE_MARK = 1;
constexpr uint8_t ATTACH_TYPE_CURSIVE = 2;

// Lookups nested inside context lookups. HarfBuzz allows 64 levels; each
// level costs a few hundred bytes of stack here and real fonts nest two or
// three deep, so fewer are allowed.
constexpr unsigned MAX_NESTING_LEVEL = 8;
// Glyphs one context rule may match (HB_MAX_CONTEXT_LENGTH).
constexpr unsigned MAX_CONTEXT_LENGTH = 64;

// The glyph string being shaped. Unlike HarfBuzz there is no separate output
// buffer: substitutions edit the one array in place, so "before idx" is the
// output HarfBuzz would have produced so far.
//
// A Buffer is meant to be reused: shape() reserves `info`, `pos` and the
// scratch arrays once for the run's maximum length, and nothing grows while
// a run is shaped (operations that would exceed the reservation fail the run
// instead of reallocating).
struct Buffer {
  std::vector<GlyphInfo> info;
  std::vector<GlyphPosition> pos;
  // Scratch for passes that rebuild the glyph string (normalization, vowel
  // constraints, dotted circles); swapped with `info`.
  std::vector<GlyphInfo> scratch;
  // Scratch for the USE syllable machine: indices of the glyphs it reads.
  std::vector<uint16_t> indices;
  unsigned idx = 0;
  // Most glyphs a run may grow to while shaping (see shape()).
  unsigned maxLength = 0;
  // Nested-lookup budget, as HarfBuzz's max_ops: stops fonts whose lookups
  // recurse without end.
  int maxOps = 0;
  bool successful = true;
  uint8_t serial = 0;        // allocates ligature IDs (HarfBuzz's next_serial)
  uint32_t randomState = 1;  // the 'rand' feature's generator
  bool hasDefaultIgnorables = false;
  bool hasBrokenSyllable = false;
  bool hasGposAttachment = false;
  // Every glyph ID the buffer holds or has held since the last refresh, so
  // lookups that cannot start at any of them are skipped outright.
  Digest digest;
  // Match positions per nesting level (row 0: the top-level lookup).
  uint16_t matchStack[MAX_NESTING_LEVEL + 1][MAX_CONTEXT_LENGTH];

  unsigned len() const { return static_cast<unsigned>(info.size()); }
  GlyphInfo& cur() { return info[idx]; }

  // Clears the run state and reserves room for `maxGlyphs`; false when the
  // reservation could not be made.
  bool prepare(unsigned maxGlyphs);

  uint8_t allocateLigId();

  // Whether `extra` more glyphs fit the reservation; marks the run failed
  // when they do not.
  bool canGrow(unsigned extra);
  // Deletes info[i] (a ligature component, an empty MultipleSubst).
  void removeGlyph(unsigned i);
  // Moves info[from] to position `to`, shifting what lies between.
  void moveGlyph(unsigned from, unsigned to);
  void reverseRange(unsigned start, unsigned end);
  void refreshDigest();
};

}  // namespace ot

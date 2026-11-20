#pragma once

#include <cstdint>

#include "OtTable.h"

namespace ot {

// GSUB and GPOS, as table indices throughout the shaper.
constexpr int GSUB = 0;
constexpr int GPOS = 1;

// The tables shaping reads. head and cmap are required; the others may be
// empty (a font without hmtx advances every glyph by half an em, as in
// HarfBuzz). CPac and CPpl hold the lookup filters and shaping plans the
// .cpfont converter precomputes (docs/file-formats.md); other fonts have none.
struct FaceTables {
  enum Index { HEAD, HHEA, MAXP, HMTX, CMAP, GDEF, GSUB_TABLE, GPOS_TABLE, CPAC, CPPL, COUNT };
  static constexpr uint32_t TAGS[COUNT] = {tag("head"), tag("hhea"), tag("maxp"), tag("hmtx"), tag("cmap"),
                                           tag("GDEF"), tag("GSUB"), tag("GPOS"), tag("CPac"), tag("CPpl")};
  Table tables[COUNT];

  const Table& operator[](const Index i) const { return tables[i]; }
  Table& operator[](const Index i) { return tables[i]; }
};

// Table views into a whole sfnt (a TrueType/OpenType file or the .cpfont
// layout font) held in memory. False when the table directory is unreadable.
bool tablesFromSfnt(const uint8_t* data, uint32_t length, FaceTables* out);

// Font-unit to output-unit scaling, as hb_font_t does it: `scale` units per
// em (26.6 fixed point in the firmware) and the pixel size hinting device
// tables are looked up at.
struct Scale {
  int32_t xScale = 0;
  int32_t yScale = 0;
  int64_t xMult = 0;
  int64_t yMult = 0;
  float xMultf = 0;
  float yMultf = 0;
  unsigned xPpem = 0;
  unsigned yPpem = 0;

  void set(int32_t scale, unsigned ppem, uint16_t upem);
  int32_t emScaleX(int32_t v) const { return static_cast<int32_t>((v * xMult + 32768) >> 16); }
  int32_t emScaleY(int32_t v) const { return static_cast<int32_t>((v * yMult + 32768) >> 16); }
  float emFscaleX(int32_t v) const { return static_cast<float>(v) * xMultf; }
  float emFscaleY(int32_t v) const { return static_cast<float>(v) * yMultf; }
};

// Glyph properties, as HarfBuzz keeps them: the GDEF class in the low bits
// (matching the LookupFlag ignore bits), the mark attachment class in the
// high byte, and what substitution did to the glyph.
namespace props {
constexpr uint16_t BASE_GLYPH = 0x02;
constexpr uint16_t LIGATURE = 0x04;
constexpr uint16_t MARK = 0x08;
constexpr uint16_t SUBSTITUTED = 0x10;
constexpr uint16_t LIGATED = 0x20;
constexpr uint16_t MULTIPLIED = 0x40;
constexpr uint16_t PRESERVE = SUBSTITUTED | LIGATED | MULTIPLIED;
}  // namespace props

// LookupFlag bits.
namespace lookupflag {
constexpr uint32_t RIGHT_TO_LEFT = 0x0001;
constexpr uint32_t IGNORE_FLAGS = 0x000E;  // ignore base glyphs, ligatures, marks
constexpr uint32_t IGNORE_MARKS = 0x0008;
constexpr uint32_t USE_MARK_FILTERING_SET = 0x0010;
constexpr uint32_t MARK_ATTACHMENT_TYPE = 0xFF00;
}  // namespace lookupflag

// One table's lookup filters in a CPac table (docs/file-formats.md).
struct FilterRecords {
  const uint8_t* lookupDigests = nullptr;    // one DIGEST_BYTES record per lookup
  Table subtableStarts;                      // per lookup, the index of its first subtable record (u32)
  const uint8_t* subtableDigests = nullptr;  // one DIGEST_BYTES record per subtable

  uint32_t firstSubtable(const uint32_t lookupIndex) const { return subtableStarts.u32(4 * lookupIndex); }
};

// One font's shaping tables, read in place (nothing is copied).
class Face {
 public:
  // False when a required table is missing.
  bool init(const FaceTables& tables);

  uint16_t upem() const { return upem_; }
  uint32_t glyphCount() const { return glyphCount_; }

  // cmap lookup; false (and *glyph = 0) when the font does not map `cp`.
  bool nominalGlyph(uint32_t cp, uint32_t* glyph) const;
  // hmtx advance in font units.
  int32_t advance(uint32_t glyph) const;

  // GDEF.
  bool hasGlyphClasses() const { return !glyphClassDef_.empty(); }
  uint16_t glyphProps(uint32_t glyph) const;
  bool markSetCovers(uint32_t set, uint32_t glyph) const;
  // Drops UseMarkFilteringSet when the set does not exist, as HarfBuzz does.
  uint32_t sanitizeLookupProps(uint32_t lookupProps) const;

  const Table& layout(const int table) const { return table == GSUB ? gsub_ : gpos_; }
  uint16_t lookupCount(int table) const;
  Table lookup(int table, uint32_t index) const;
  // Subtables of a lookup whose offsets lie inside the table: its declared
  // count unless the font is malformed.
  static uint16_t subtableCount(const Table& lookup) { return lookup.count16(4, 6, 2); }
  // LookupFlag, plus the mark filtering set in the high 16 bits when used.
  uint32_t lookupProps(const Table& lookup) const;
  // Precomputed lookup filters of a table; nullptr when the font has none or
  // they do not match its lookups.
  const FilterRecords* filters(const int table) const {
    return filters_[table].lookupDigests ? &filters_[table] : nullptr;
  }
  // Precompiled shaping plans (the CPpl table); empty when the font has none.
  const Table& compiledPlans() const { return compiledPlans_; }

 private:
  void initFilters(const Table& cpac);

  Table cmapSubtable_;
  uint16_t cmapFormat_ = 0;
  Table hmtx_;
  uint16_t longMetrics_ = 0;
  uint32_t bearingCount_ = 0;
  uint16_t upem_ = 1000;
  uint32_t glyphCount_ = 0;

  Table glyphClassDef_;
  Table markAttachClassDef_;
  Table markGlyphSets_;
  uint16_t markGlyphSetCount_ = 0;

  Table gsub_;
  Table gpos_;
  FilterRecords filters_[2];
  Table compiledPlans_;
};

}  // namespace ot

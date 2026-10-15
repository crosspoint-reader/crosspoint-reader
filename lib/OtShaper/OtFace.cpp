#include "OtFace.h"

#include "OtBuffer.h"
#include "OtLayoutInternal.h"

namespace ot {

namespace {

constexpr uint32_t DIGEST_RECORD = DIGEST_BYTES;

// The cmap subtable for (platform, encoding), found by binary search as the
// encoding records are sorted.
Table findCmapSubtable(const Table& cmap, const uint16_t platform, const uint16_t encoding) {
  const uint32_t key = (static_cast<uint32_t>(platform) << 16) | encoding;
  int lo = 0;
  int hi = static_cast<int>(cmap.u16(2)) - 1;
  while (lo <= hi) {
    const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
    const uint32_t record = 4 + 8 * mid;
    const uint32_t k = (static_cast<uint32_t>(cmap.u16(record)) << 16) | cmap.u16(record + 2);
    if (key < k) {
      hi = mid - 1;
    } else if (key > k) {
      lo = mid + 1;
    } else {
      return cmap.offset32(record + 4);
    }
  }
  return Table();
}

bool format4Glyph(const Table& t, const uint32_t cp, uint32_t* glyph) {
  if (cp > 0xFFFF) return false;
  const uint32_t segCount = t.u16(6) / 2;
  const uint32_t endCodes = 14;
  const uint32_t startCodes = endCodes + 2 * segCount + 2;
  const uint32_t idDeltas = startCodes + 2 * segCount;
  const uint32_t idRangeOffsets = idDeltas + 2 * segCount;
  const uint32_t glyphIds = idRangeOffsets + 2 * segCount;
  int lo = 0;
  int hi = static_cast<int>(segCount) - 1;
  while (lo <= hi) {
    const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
    if (cp > t.u16(endCodes + 2 * mid)) {
      lo = mid + 1;
    } else if (cp < t.u16(startCodes + 2 * mid)) {
      hi = mid - 1;
    } else {
      const uint16_t rangeOffset = t.u16(idRangeOffsets + 2 * mid);
      const uint16_t delta = t.u16(idDeltas + 2 * mid);
      uint32_t gid;
      if (rangeOffset == 0) {
        gid = cp + delta;
      } else {
        const uint32_t index = rangeOffset / 2 + (cp - t.u16(startCodes + 2 * mid)) + mid - segCount;
        const uint32_t glyphIdCount = (t.u16(2) > glyphIds ? t.u16(2) - glyphIds : 0) / 2;
        if (index >= glyphIdCount) return false;
        gid = t.u16(glyphIds + 2 * index);
        if (gid == 0) return false;
        gid += delta;
      }
      gid &= 0xFFFF;
      if (gid == 0) return false;
      *glyph = gid;
      return true;
    }
  }
  return false;
}

bool format12Glyph(const Table& t, const uint32_t cp, uint32_t* glyph, const bool constant) {
  int lo = 0;
  int hi = static_cast<int>(t.u32(12)) - 1;
  while (lo <= hi) {
    const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
    const uint32_t group = 16 + 12 * static_cast<uint32_t>(mid);
    const uint32_t start = t.u32(group);
    const uint32_t end = t.u32(group + 4);
    if (cp < start) {
      hi = mid - 1;
    } else if (cp > end) {
      lo = mid + 1;
    } else {
      const uint32_t gid = constant ? t.u32(group + 8) : t.u32(group + 8) + (cp - start);
      if (gid == 0) return false;
      *glyph = gid;
      return true;
    }
  }
  return false;
}

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

bool tablesFromSfnt(const uint8_t* data, const uint32_t length, FaceTables* out) {
  *out = FaceTables{};
  if (data == nullptr || length < 12) return false;
  const unsigned count = (data[4] << 8) | data[5];
  for (unsigned i = 0; i < count; i++) {
    if (12 + 16u * (i + 1) > length) return false;
    const uint8_t* record = data + 12 + 16 * i;
    const uint32_t offset = be32(record + 8);
    const uint32_t size = be32(record + 12);
    if (offset > length || size > length - offset) continue;
    for (int t = 0; t < FaceTables::COUNT; t++) {
      if (FaceTables::TAGS[t] == be32(record)) out->tables[t] = Table(data + offset, size);
    }
  }
  return true;
}

void Scale::set(const int32_t scale, const unsigned ppem, const uint16_t upem) {
  xScale = yScale = scale;
  xMultf = yMultf = static_cast<float>(scale) / static_cast<float>(upem);
  xMult = yMult = static_cast<int64_t>(scale) * 0x10000 / upem;
  xPpem = yPpem = ppem;
}

bool Face::init(const FaceTables& tables) {
  const Table& head = tables[FaceTables::HEAD];
  const Table& cmap = tables[FaceTables::CMAP];
  if (head.empty() || cmap.empty()) return false;
  const uint16_t upem = head.u16(18);
  upem_ = upem >= 16 && upem <= 16384 ? upem : 1000;

  // Same subtable preference as HarfBuzz: full-repertoire Unicode first.
  static constexpr uint16_t PREFERENCE[][2] = {{3, 10}, {0, 6}, {0, 4}, {3, 1}, {0, 3}, {0, 2}, {0, 1}, {0, 0}};
  for (const auto& pe : PREFERENCE) {
    cmapSubtable_ = findCmapSubtable(cmap, pe[0], pe[1]);
    if (!cmapSubtable_.empty()) break;
  }
  cmapFormat_ = cmapSubtable_.u16(0);

  glyphCount_ = tables[FaceTables::MAXP].u16(4);
  hmtx_ = tables[FaceTables::HMTX];
  longMetrics_ = tables[FaceTables::HHEA].u16(34);
  if (static_cast<uint32_t>(longMetrics_) * 4 > hmtx_.length()) {
    longMetrics_ = static_cast<uint16_t>(hmtx_.length() / 4);
  }
  const uint32_t bearingCapacity = longMetrics_ + (hmtx_.length() - 4u * longMetrics_) / 2;
  bearingCount_ = glyphCount_ < bearingCapacity ? glyphCount_ : bearingCapacity;

  const Table& gdef = tables[FaceTables::GDEF];
  glyphClassDef_ = gdef.offset16(4);
  markAttachClassDef_ = gdef.offset16(10);
  if (gdef.u32(0) >= 0x00010002) {
    markGlyphSets_ = gdef.offset16(12);
    markGlyphSetCount_ = markGlyphSets_.u16(0) == 1 ? markGlyphSets_.u16(2) : 0;
  }

  const Table& gsub = tables[FaceTables::GSUB_TABLE];
  const Table& gpos = tables[FaceTables::GPOS_TABLE];
  gsub_ = gsub.u16(0) == 1 ? gsub : Table();
  gpos_ = gpos.u16(0) == 1 ? gpos : Table();
  initFilters(tables[FaceTables::CPAC]);
  compiledPlans_ = tables[FaceTables::CPPL];
  return true;
}

// CPac (docs/file-formats.md): u16 version (1), u16 reserved, u16 lookup
// count per table (GSUB, GPOS), u32 subtable count per table, then per table
// the lookup digests, the u32 first-subtable index per lookup and the
// subtable digests. Used only when every count and index matches the font's
// own lookups, so no stored value can point outside the table.
void Face::initFilters(const Table& cpac) {
  filters_[GSUB] = filters_[GPOS] = FilterRecords{};
  if (cpac.u16(0) != 1) return;
  uint64_t offset = 16;
  FilterRecords found[2];
  for (int t = GSUB; t <= GPOS; t++) {
    const uint32_t lookups = cpac.u16(4 + 2 * t);
    const uint32_t subtables = cpac.u32(8 + 4 * t);
    if (lookups != lookupCount(t)) return;
    const uint64_t size =
        static_cast<uint64_t>(lookups) * (DIGEST_RECORD + 4) + static_cast<uint64_t>(subtables) * DIGEST_RECORD;
    if (offset + size > cpac.length()) return;
    const auto base = static_cast<uint32_t>(offset);
    const Table starts = Table(cpac.data() + base + lookups * DIGEST_RECORD, lookups * 4);
    uint32_t total = 0;
    for (uint32_t i = 0; i < lookups; i++) {
      if (starts.u32(4 * i) != total) return;
      total += subtableCount(lookup(t, i));
    }
    if (total != subtables) return;
    found[t].lookupDigests = cpac.data() + base;
    found[t].subtableStarts = starts;
    found[t].subtableDigests = cpac.data() + base + lookups * (DIGEST_RECORD + 4);
    offset += size;
  }
  filters_[GSUB] = found[GSUB];
  filters_[GPOS] = found[GPOS];
}

bool Face::nominalGlyph(const uint32_t cp, uint32_t* glyph) const {
  *glyph = 0;
  const Table& t = cmapSubtable_;
  switch (cmapFormat_) {
    case 0:
      if (cp > 0xFF) return false;
      *glyph = t.u8(6 + cp);
      return *glyph != 0;
    case 4:
      return format4Glyph(t, cp, glyph);
    case 6: {
      const uint16_t first = t.u16(6);
      if (cp < first || cp - first >= t.u16(8)) return false;
      *glyph = t.u16(10 + 2 * (cp - first));
      return *glyph != 0;
    }
    case 12:
      return format12Glyph(t, cp, glyph, false);
    case 13:
      return format12Glyph(t, cp, glyph, true);
    default:
      return false;
  }
}

int32_t Face::advance(const uint32_t glyph) const {
  if (longMetrics_ == 0) return upem_ / 2;
  if (glyph < bearingCount_) {
    const uint32_t metric = glyph < longMetrics_ ? glyph : longMetrics_ - 1u;
    return hmtx_.u16(4 * metric);
  }
  return 0;
}

uint16_t Face::glyphProps(const uint32_t glyph) const {
  switch (classOf(glyphClassDef_, glyph)) {
    case 1:
      return props::BASE_GLYPH;
    case 2:
      return props::LIGATURE;
    case 3:
      return static_cast<uint16_t>(props::MARK | (classOf(markAttachClassDef_, glyph) << 8));
    default:
      return 0;
  }
}

bool Face::markSetCovers(const uint32_t set, const uint32_t glyph) const {
  if (set >= markGlyphSetCount_) return false;
  return coverageIndex(markGlyphSets_.offset32(4 + 4 * set), glyph) != NOT_COVERED;
}

uint32_t Face::sanitizeLookupProps(uint32_t lookupProps) const {
  if ((lookupProps & lookupflag::USE_MARK_FILTERING_SET) && (lookupProps >> 16) >= markGlyphSetCount_) {
    lookupProps &= ~lookupflag::USE_MARK_FILTERING_SET;
  }
  return lookupProps;
}

uint16_t Face::lookupCount(const int table) const { return layout(table).offset16(8).u16(0); }

Table Face::lookup(const int table, const uint32_t index) const {
  const Table list = layout(table).offset16(8);
  if (index >= list.u16(0)) return Table();
  const Table lookup = list.offset16(2 + 2 * index);
  // Every subtable of an Extension lookup must extend the same type, or
  // HarfBuzz's Lookup::sanitize drops the lookup: a reverse chaining subtable
  // applied in a forward pass (or the reverse) would never move past its glyph.
  if (lookup.u16(0) == (table == GSUB ? layout::gsub::EXTENSION : layout::gpos::EXTENSION)) {
    const auto extendedType = [&](const uint16_t i) {
      const Table st = lookup.offset16(6 + 2 * i);
      return st.u16(0) == 1 ? st.u16(2) : 0;
    };
    const uint16_t first = extendedType(0);
    for (uint16_t i = 1; i < subtableCount(lookup); i++) {
      if (extendedType(i) != first) return Table();
    }
  }
  return lookup;
}

uint32_t Face::lookupProps(const Table& lookup) const {
  const uint16_t flag = lookup.u16(2);
  uint32_t result = flag;
  if (flag & lookupflag::USE_MARK_FILTERING_SET) {
    const uint16_t subtableCount = lookup.u16(4);
    result |= static_cast<uint32_t>(lookup.u16(6 + 2 * subtableCount)) << 16;
  }
  return result;
}

}  // namespace ot

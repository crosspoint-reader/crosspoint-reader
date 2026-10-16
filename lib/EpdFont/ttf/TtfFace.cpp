#include "TtfFace.h"

#include "TtfFixed.h"

namespace ttf {

namespace {

constexpr uint32_t tag(const char (&name)[5]) {
  return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) << 24 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 8 | static_cast<uint8_t>(name[3]);
}

constexpr uint32_t kVersionTrueType = 0x00010000;
constexpr uint32_t kVersionApple = tag("true");
constexpr uint32_t kVersionCff = tag("OTTO");
constexpr uint32_t kVersionCollection = tag("ttcf");
constexpr uint32_t kMaxpVersion1 = 0x00010000;  // 0.5 is the CFF form, without the outline limits

struct Table {
  uint32_t offset = 0;
  uint32_t length = 0;
};

// The table `wanted`, when the directory lists it, it lies inside the file and it holds `minLength` bytes.
bool findTable(FontBytes& bytes, const uint16_t numTables, const uint32_t wanted, const uint32_t minLength,
               Table* out) {
  for (uint32_t i = 0; i < numTables; ++i) {
    const uint32_t record = 12 + 16 * i;
    if (bytes.u32(record) != wanted) continue;
    out->offset = bytes.u32(record + 8);
    out->length = bytes.u32(record + 12);
    return static_cast<uint64_t>(out->offset) + out->length <= bytes.size() && out->length >= minLength;
  }
  return false;
}

// FreeType reads OS/2 field groups by version and treats a table too short for its version as missing.
uint32_t os2Length(const uint16_t version) {
  if (version >= 5) return 100;
  if (version >= 2) return 96;
  if (version >= 1) return 86;
  return 78;
}

FaceStatus readFailure(const FontBytes& bytes, const FaceStatus otherwise) {
  return bytes.ioError() ? FaceStatus::Transient : otherwise;
}

// FreeType's choice of Unicode map (find_unicode_charmap): from the last record backwards, first a UCS-4 map,
// then any Unicode map. Only formats 4 and 12 are read here, so other subtables are passed over.
bool chooseCmap(FontBytes& bytes, const Table& cmap, Face* face) {
  const uint16_t records = bytes.u16(cmap.offset + 2);
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = records; i-- > 0;) {
      const uint32_t record = 4 + 8 * i;
      if (record + 8 > cmap.length) continue;  // FreeType drops records past the table
      const uint16_t platform = bytes.u16(cmap.offset + record);
      const uint16_t encoding = bytes.u16(cmap.offset + record + 2);
      const uint32_t offset = bytes.u32(cmap.offset + record + 4);
      const bool ucs4 = (platform == 3 && encoding == 10) || (platform == 0 && encoding == 4);
      const bool unicode = platform == 0 || platform == 2 || (platform == 3 && (encoding == 1 || encoding == 10));
      if (pass == 0 ? !ucs4 : !unicode) continue;
      if (platform == 0 && encoding == 5) continue;  // variation sequences, not a character map
      if (offset == 0 || offset > cmap.length - 2) continue;
      const uint16_t format = bytes.u16(cmap.offset + offset);
      if (format != 4 && format != 12) continue;
      face->cmapSubtable = cmap.offset + offset;
      face->cmapFormat = static_cast<uint8_t>(format);
      return true;
    }
  }
  return false;
}

uint32_t format12Glyph(FontBytes& bytes, const Face& face, const uint32_t codepoint) {
  const uint32_t subtable = face.cmapSubtable;
  const uint32_t room = face.cmapEnd > subtable + 16 ? (face.cmapEnd - subtable - 16) / 12 : 0;
  const uint32_t groups = bytes.u32(subtable + 12);
  uint32_t lo = 0;
  uint32_t hi = groups < room ? groups : room;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint32_t group = subtable + 16 + 12 * mid;
    const uint32_t start = bytes.u32(group);
    if (codepoint < start) {
      hi = mid;
    } else if (codepoint > bytes.u32(group + 4)) {
      lo = mid + 1;
    } else {
      const uint32_t first = bytes.u32(group + 8);
      return first > 0xFFFFFFFFu - (codepoint - start) ? 0 : first + (codepoint - start);
    }
  }
  return 0;
}

uint32_t format4Glyph(FontBytes& bytes, const Face& face, const uint32_t codepoint) {
  if (codepoint > 0xFFFF) return 0;
  const uint32_t subtable = face.cmapSubtable;
  const uint32_t segX2 = bytes.u16(subtable + 6) & ~1u;
  const uint32_t ends = subtable + 14;
  const uint32_t starts = ends + segX2 + 2;  // past the reserved pad
  const uint32_t deltas = starts + segX2;
  const uint32_t rangeOffsets = deltas + segX2;
  uint32_t lo = 0;
  uint32_t hi = segX2 / 2;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    const uint32_t start = bytes.u16(starts + 2 * mid);
    if (codepoint < start) {
      hi = mid;
    } else if (codepoint > bytes.u16(ends + 2 * mid)) {
      lo = mid + 1;
    } else {
      const int32_t delta = bytes.i16(deltas + 2 * mid);
      const uint32_t rangePos = rangeOffsets + 2 * mid;
      const uint16_t rangeOffset = bytes.u16(rangePos);
      if (rangeOffset == 0xFFFF) return 0;  // FreeType reads this broken marker as "no glyph"
      if (rangeOffset == 0) return static_cast<uint32_t>(static_cast<int32_t>(codepoint) + delta) & 0xFFFF;
      const uint16_t glyph = bytes.u16(rangePos + rangeOffset + 2 * (codepoint - start));
      return glyph == 0 ? 0 : static_cast<uint32_t>(glyph + delta) & 0xFFFF;
    }
  }
  return 0;
}

}  // namespace

FaceStatus parseFace(FontBytes& bytes, Face* face) {
  *face = Face{};
  const uint32_t version = bytes.u32(0);
  if (version == kVersionCff || version == kVersionCollection) return FaceStatus::Unsupported;
  if (version != kVersionTrueType && version != kVersionApple) return readFailure(bytes, FaceStatus::Malformed);
  const uint16_t numTables = bytes.u16(4);
  if (12 + 16 * static_cast<uint64_t>(numTables) > bytes.size()) return readFailure(bytes, FaceStatus::Malformed);

  Table head, hhea, maxp, cmap, loca, glyf, hmtx;
  if (!findTable(bytes, numTables, tag("head"), 54, &head) || !findTable(bytes, numTables, tag("hhea"), 36, &hhea) ||
      !findTable(bytes, numTables, tag("maxp"), 32, &maxp) || !findTable(bytes, numTables, tag("cmap"), 4, &cmap) ||
      !findTable(bytes, numTables, tag("loca"), 0, &loca) || !findTable(bytes, numTables, tag("glyf"), 0, &glyf) ||
      !findTable(bytes, numTables, tag("hmtx"), 0, &hmtx)) {
    return readFailure(bytes, FaceStatus::Malformed);
  }

  face->unitsPerEm = bytes.u16(head.offset + 18);
  const int16_t locaFormat = bytes.i16(head.offset + 50);
  face->numGlyphs = bytes.u16(maxp.offset + 4);
  face->numHMetrics = bytes.u16(hhea.offset + 34);
  if (face->unitsPerEm < 16 || face->unitsPerEm > 16384 || (locaFormat != 0 && locaFormat != 1) ||
      bytes.u32(maxp.offset) != kMaxpVersion1 || face->numGlyphs == 0 || face->numHMetrics == 0) {
    return readFailure(bytes, FaceStatus::Malformed);
  }
  face->longLoca = locaFormat == 1;
  face->xMin = bytes.i16(head.offset + 36);
  face->yMin = bytes.i16(head.offset + 38);
  face->xMax = bytes.i16(head.offset + 40);
  face->yMax = bytes.i16(head.offset + 42);
  const uint16_t simplePoints = bytes.u16(maxp.offset + 6);
  const uint16_t compositePoints = bytes.u16(maxp.offset + 10);
  const uint16_t simpleContours = bytes.u16(maxp.offset + 8);
  const uint16_t compositeContours = bytes.u16(maxp.offset + 12);
  face->maxPoints = simplePoints > compositePoints ? simplePoints : compositePoints;
  face->maxContours = simpleContours > compositeContours ? simpleContours : compositeContours;
  face->glyf = glyf.offset;
  face->glyfLength = glyf.length;
  face->loca = loca.offset;
  face->locaLength = loca.length;
  face->hmtx = hmtx.offset;
  face->hmtxLength = hmtx.length;

  // FreeType's face metrics (sfnt_load_face): OS/2's typo metrics when it sets USE_TYPO_METRICS, else hhea's;
  // when hhea's are both 0, OS/2's typo metrics, or its win metrics when those are 0 too.
  Table os2;
  bool haveOs2 = findTable(bytes, numTables, tag("OS/2"), 78, &os2);
  if (haveOs2) haveOs2 = os2.length >= os2Length(bytes.u16(os2.offset));
  const int16_t typoAscender = haveOs2 ? bytes.i16(os2.offset + 68) : 0;
  const int16_t typoDescender = haveOs2 ? bytes.i16(os2.offset + 70) : 0;
  const int16_t typoLineGap = haveOs2 ? bytes.i16(os2.offset + 72) : 0;
  int32_t ascender = bytes.i16(hhea.offset + 4);
  int32_t descender = bytes.i16(hhea.offset + 6);
  int32_t height = ascender - descender + bytes.i16(hhea.offset + 8);
  const bool useTypo = haveOs2 && (bytes.u16(os2.offset + 62) & 0x80) != 0;
  if (useTypo || (ascender == 0 && descender == 0 && haveOs2 && (typoAscender != 0 || typoDescender != 0))) {
    ascender = typoAscender;
    descender = typoDescender;
    height = ascender - descender + typoLineGap;
  } else if (ascender == 0 && descender == 0 && haveOs2) {
    ascender = bytes.u16(os2.offset + 74);
    descender = -static_cast<int32_t>(bytes.u16(os2.offset + 76));
    height = ascender - descender;
  }
  face->ascender = static_cast<int16_t>(ascender);
  face->descender = static_cast<int16_t>(descender);
  face->height = static_cast<int16_t>(height);

  // Optional: a font without them draws without kerning or ligatures.
  Table layout;
  if (findTable(bytes, numTables, tag("GPOS"), 10, &layout)) {
    face->gpos = layout.offset;
    face->gposLength = layout.length;
  }
  if (findTable(bytes, numTables, tag("GSUB"), 10, &layout)) {
    face->gsub = layout.offset;
    face->gsubLength = layout.length;
  }
  if (findTable(bytes, numTables, tag("kern"), 4, &layout)) {
    face->kern = layout.offset;
    face->kernLength = layout.length;
  }

  face->cmapEnd = cmap.offset + cmap.length;
  if (!chooseCmap(bytes, cmap, face)) return readFailure(bytes, FaceStatus::Unsupported);
  if (bytes.ioError()) return FaceStatus::Transient;
  return bytes.outOfRange() ? FaceStatus::Malformed : FaceStatus::Ok;
}

uint16_t glyphIndex(FontBytes& bytes, const Face& face, const uint32_t codepoint) {
  const uint32_t glyph =
      face.cmapFormat == 12 ? format12Glyph(bytes, face, codepoint) : format4Glyph(bytes, face, codepoint);
  return glyph < face.numGlyphs ? static_cast<uint16_t>(glyph) : 0;
}

int32_t unitsToPixels(const Face& face, const uint32_t ppem26_6) {
  return divFix(static_cast<int32_t>(ppem26_6), face.unitsPerEm);
}

void horizontalMetrics(FontBytes& bytes, const Face& face, const uint16_t glyph, uint16_t* advance, int16_t* lsb) {
  *advance = 0;
  *lsb = 0;
  const uint32_t metrics = face.numHMetrics;
  if (glyph < metrics) {
    const uint32_t pos = 4 * static_cast<uint32_t>(glyph);
    if (pos + 4 > face.hmtxLength) return;
    *advance = bytes.u16(face.hmtx + pos);
    *lsb = bytes.i16(face.hmtx + pos + 2);
    return;
  }
  const uint32_t last = 4 * (metrics - 1);
  if (last + 2 > face.hmtxLength) return;
  *advance = bytes.u16(face.hmtx + last);
  const uint32_t pos = 4 * metrics + 2 * (glyph - metrics);
  if (pos + 2 <= face.hmtxLength) *lsb = bytes.i16(face.hmtx + pos);
}

uint32_t advance12_4(FontBytes& bytes, const Face& face, const uint16_t glyph, const uint32_t ppem26_6) {
  uint16_t advance = 0;
  int16_t lsb = 0;
  horizontalMetrics(bytes, face, glyph, &advance, &lsb);
  // FreeType: linearHoriAdvance = FT_MulDiv(advance, x_scale, 64) in 16.16; the converter rounds that to 12.4.
  // In 64 bits: a lying hmtx on a small em passes 2^31 (at most 2^36: 65,535 units x a 2^26 scale / 64).
  const int64_t linear = mulDivWide(advance, unitsToPixels(face, ppem26_6), 64);
  return static_cast<uint32_t>((linear + (1 << 11)) >> 12);
}

LineMetrics lineMetrics(const Face& face, const uint32_t ppem26_6) {
  const int32_t scale = unitsToPixels(face, ppem26_6);
  // FreeType's size metrics (ft_recompute_scaled_metrics): height rounded, ascender up, descender down.
  const int32_t height = (mulFix(face.height, scale) + 32) & -64;
  const int32_t ascender = (mulFix(face.ascender, scale) + 63) & -64;
  const int32_t descender = mulFix(face.descender, scale) & -64;
  return {height / 64, ascender / 64, descender / 64};
}

}  // namespace ttf

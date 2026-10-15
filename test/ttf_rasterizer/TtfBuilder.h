#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

/// Small TrueType fonts built in memory for the TTF engine's tests: the tables the engine reads, laid out as a
/// font tool writes them (checksums left 0; nothing reads them).
namespace ttf_test {

using Bytes = std::vector<uint8_t>;

inline void put16(Bytes& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}
inline void put32(Bytes& out, const uint32_t value) {
  put16(out, value >> 16);
  put16(out, value & 0xFFFF);
}
inline void set16(Bytes& out, const size_t at, const uint32_t value) {
  out[at] = static_cast<uint8_t>(value >> 8);
  out[at + 1] = static_cast<uint8_t>(value);
}
inline void set32(Bytes& out, const size_t at, const uint32_t value) {
  set16(out, at, value >> 16);
  set16(out, at + 2, value & 0xFFFF);
}

struct Point {
  int16_t x = 0;
  int16_t y = 0;
  bool on = true;
};

/// A component of a compound glyph. `transform` holds F2Dot14 values: none, 1 (a scale), 2 (x and y scales) or
/// 4 (a 2x2 matrix in file order). `flags` adds bits to the ones the builder sets itself.
struct Component {
  Component(const uint16_t glyph, const int16_t dx, const int16_t dy, std::vector<int16_t> transform = {},
            const uint16_t flags = 0)
      : glyph(glyph), dx(dx), dy(dy), transform(std::move(transform)), flags(flags) {}

  uint16_t glyph;
  int16_t dx;
  int16_t dy;
  std::vector<int16_t> transform;
  uint16_t flags;
};

/// One glyph: a simple outline, a compound, or `raw` bytes taken as the whole glyf record.
struct Glyph {
  std::vector<std::vector<Point>> contours;
  std::vector<Component> components;
  Bytes raw;
  uint16_t advance = 500;
  bool lsbIsXMin = true;  ///< otherwise `lsb`
  int16_t lsb = 0;
};

struct Font {
  uint32_t sfntVersion = 0x00010000;
  uint16_t unitsPerEm = 1000;
  uint16_t macStyle = 0;  ///< head.macStyle: bit 0 bold, bit 1 italic
  int16_t ascender = 800;
  int16_t descender = -200;
  int16_t lineGap = 0;
  bool longLoca = false;
  std::vector<Glyph> glyphs;  ///< glyph 0 is .notdef
  std::map<uint32_t, uint16_t> cmap;
  bool cmapFormat12 = false;      ///< a (3,10) format 12 subtable instead of (3,1) format 4
  bool cmapRangeOffsets = false;  ///< format 4 through glyphIdArray instead of deltas
  uint16_t numHMetrics = 0;       ///< 0: one hmtx entry per glyph
  bool os2 = false;
  uint16_t weightClass = 400;  ///< OS/2 usWeightClass
  uint16_t os2Selection = 0;
  int16_t typoAscender = 0;
  int16_t typoDescender = 0;
  int16_t typoLineGap = 0;
  uint16_t winAscent = 0;
  uint16_t winDescent = 0;
  std::vector<std::pair<std::string, Bytes>> extraTables;  ///< added or replacing a built table
};

inline uint32_t tagOf(const std::string& name) {
  return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) << 24 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 8 | static_cast<uint8_t>(name[3]);
}

struct Box {
  int16_t xMin = 0, yMin = 0, xMax = 0, yMax = 0;
};

inline Box simpleBox(const Glyph& glyph) {
  Box box;
  bool first = true;
  for (const auto& contour : glyph.contours) {
    for (const Point& p : contour) {
      if (first) {
        box = {p.x, p.y, p.x, p.y};
        first = false;
      }
      box.xMin = std::min(box.xMin, p.x);
      box.yMin = std::min(box.yMin, p.y);
      box.xMax = std::max(box.xMax, p.x);
      box.yMax = std::max(box.yMax, p.y);
    }
  }
  return box;
}

// A simple glyph with the short, same and repeat encodings a font tool uses.
inline Bytes encodeSimple(const Glyph& glyph) {
  Bytes out;
  const Box box = simpleBox(glyph);
  put16(out, static_cast<uint16_t>(glyph.contours.size()));
  put16(out, static_cast<uint16_t>(box.xMin));
  put16(out, static_cast<uint16_t>(box.yMin));
  put16(out, static_cast<uint16_t>(box.xMax));
  put16(out, static_cast<uint16_t>(box.yMax));
  std::vector<Point> points;
  for (const auto& contour : glyph.contours) {
    points.insert(points.end(), contour.begin(), contour.end());
    put16(out, static_cast<uint16_t>(points.size() - 1));
  }
  put16(out, 0);  // no instructions
  std::vector<uint8_t> flags;
  Bytes xs, ys;
  int16_t lastX = 0, lastY = 0;
  for (const Point& p : points) {
    uint8_t flag = p.on ? 0x01 : 0x00;
    const int dx = p.x - lastX, dy = p.y - lastY;
    if (dx == 0) {
      flag |= 0x10;
    } else if (dx >= -255 && dx <= 255) {
      flag |= 0x02 | (dx > 0 ? 0x10 : 0);
      xs.push_back(static_cast<uint8_t>(dx > 0 ? dx : -dx));
    } else {
      put16(xs, static_cast<uint16_t>(dx));
    }
    if (dy == 0) {
      flag |= 0x20;
    } else if (dy >= -255 && dy <= 255) {
      flag |= 0x04 | (dy > 0 ? 0x20 : 0);
      ys.push_back(static_cast<uint8_t>(dy > 0 ? dy : -dy));
    } else {
      put16(ys, static_cast<uint16_t>(dy));
    }
    flags.push_back(flag);
    lastX = p.x;
    lastY = p.y;
  }
  for (size_t i = 0; i < flags.size();) {
    size_t run = 1;
    while (i + run < flags.size() && flags[i + run] == flags[i] && run < 256) ++run;
    if (run > 1) {
      out.push_back(flags[i] | 0x08);
      out.push_back(static_cast<uint8_t>(run - 1));
    } else {
      out.push_back(flags[i]);
    }
    i += run;
  }
  out.insert(out.end(), xs.begin(), xs.end());
  out.insert(out.end(), ys.begin(), ys.end());
  return out;
}

inline Bytes encodeCompound(const Glyph& glyph, const Box& box) {
  Bytes out;
  put16(out, 0xFFFF);  // -1 contours
  put16(out, static_cast<uint16_t>(box.xMin));
  put16(out, static_cast<uint16_t>(box.yMin));
  put16(out, static_cast<uint16_t>(box.xMax));
  put16(out, static_cast<uint16_t>(box.yMax));
  for (size_t i = 0; i < glyph.components.size(); ++i) {
    const Component& c = glyph.components[i];
    const bool words = c.dx < -128 || c.dx > 127 || c.dy < -128 || c.dy > 127;
    uint16_t flags = static_cast<uint16_t>(c.flags | 0x0002);  // ARGS_ARE_XY_VALUES
    if (words) flags |= 0x0001;
    if (i + 1 < glyph.components.size()) flags |= 0x0020;
    if (c.transform.size() == 1) flags |= 0x0008;
    if (c.transform.size() == 2) flags |= 0x0040;
    if (c.transform.size() == 4) flags |= 0x0080;
    put16(out, flags);
    put16(out, c.glyph);
    if (words) {
      put16(out, static_cast<uint16_t>(c.dx));
      put16(out, static_cast<uint16_t>(c.dy));
    } else {
      out.push_back(static_cast<uint8_t>(c.dx));
      out.push_back(static_cast<uint8_t>(c.dy));
    }
    for (const int16_t v : c.transform) put16(out, static_cast<uint16_t>(v));
  }
  return out;
}

inline Bytes cmapFormat4(const std::map<uint32_t, uint16_t>& map, const bool rangeOffsets) {
  // Segments of consecutive codepoints (and, for deltas, consecutive glyphs), then the 0xFFFF segment.
  struct Segment {
    uint16_t start, end;
    std::vector<uint16_t> glyphs;
  };
  std::vector<Segment> segments;
  for (const auto& [cp, glyph] : map) {
    if (cp > 0xFFFF) continue;
    if (!segments.empty() && segments.back().end + 1u == cp &&
        (rangeOffsets || segments.back().glyphs.back() + 1u == glyph)) {
      segments.back().end = static_cast<uint16_t>(cp);
      segments.back().glyphs.push_back(glyph);
    } else {
      segments.push_back({static_cast<uint16_t>(cp), static_cast<uint16_t>(cp), {glyph}});
    }
  }
  segments.push_back({0xFFFF, 0xFFFF, {0}});
  const uint16_t n = static_cast<uint16_t>(segments.size());
  Bytes out;
  put16(out, 4);
  put16(out, 0);  // length, set below
  put16(out, 0);  // language
  put16(out, n * 2);
  put16(out, 0);  // searchRange, entrySelector, rangeShift: nothing reads them
  put16(out, 0);
  put16(out, 0);
  for (const Segment& s : segments) put16(out, s.end);
  put16(out, 0);
  for (const Segment& s : segments) put16(out, s.start);
  for (const Segment& s : segments) {
    const bool last = s.start == 0xFFFF;
    put16(out, rangeOffsets || last ? (last ? 1 : 0) : static_cast<uint16_t>(s.glyphs[0] - s.start));
  }
  // idRangeOffset: bytes from this entry to the segment's first glyphIdArray entry.
  uint32_t arrayIndex = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const bool last = segments[i].start == 0xFFFF;
    if (!rangeOffsets || last) {
      put16(out, 0);
      continue;
    }
    put16(out, static_cast<uint16_t>(2 * (n - i) + 2 * arrayIndex));
    arrayIndex += static_cast<uint32_t>(segments[i].glyphs.size());
  }
  if (rangeOffsets) {
    for (const Segment& s : segments) {
      if (s.start == 0xFFFF) continue;
      for (const uint16_t glyph : s.glyphs) put16(out, glyph);
    }
  }
  set16(out, 2, static_cast<uint32_t>(out.size()));
  return out;
}

inline Bytes cmapFormat12(const std::map<uint32_t, uint16_t>& map) {
  std::vector<std::array<uint32_t, 3>> groups;
  for (const auto& [cp, glyph] : map) {
    if (!groups.empty() && groups.back()[1] + 1 == cp && groups.back()[2] + (cp - groups.back()[0]) == glyph) {
      groups.back()[1] = cp;
    } else {
      groups.push_back({cp, cp, glyph});
    }
  }
  Bytes out;
  put16(out, 12);
  put16(out, 0);
  put32(out, static_cast<uint32_t>(16 + 12 * groups.size()));
  put32(out, 0);
  put32(out, static_cast<uint32_t>(groups.size()));
  for (const auto& g : groups) {
    put32(out, g[0]);
    put32(out, g[1]);
    put32(out, g[2]);
  }
  return out;
}

/// A cmap table from (platform, encoding, subtable) records, in the order given.
inline Bytes cmapTable(const std::vector<std::tuple<uint16_t, uint16_t, Bytes>>& subtables) {
  Bytes out;
  put16(out, 0);
  put16(out, static_cast<uint16_t>(subtables.size()));
  uint32_t offset = 4 + 8 * static_cast<uint32_t>(subtables.size());
  for (const auto& [platform, encoding, data] : subtables) {
    put16(out, platform);
    put16(out, encoding);
    put32(out, offset);
    offset += static_cast<uint32_t>(data.size());
  }
  for (const auto& entry : subtables) {
    const Bytes& data = std::get<2>(entry);
    out.insert(out.end(), data.begin(), data.end());
  }
  return out;
}

inline Bytes buildFont(const Font& font) {
  // glyf and loca, and each glyph's box (compound boxes from their components, one level deep).
  Bytes glyf;
  std::vector<uint32_t> offsets;
  std::vector<Box> boxes(font.glyphs.size());
  for (size_t g = 0; g < font.glyphs.size(); ++g) boxes[g] = simpleBox(font.glyphs[g]);
  for (size_t g = 0; g < font.glyphs.size(); ++g) {
    const Glyph& glyph = font.glyphs[g];
    offsets.push_back(static_cast<uint32_t>(glyf.size()));
    Bytes record;
    if (!glyph.raw.empty()) {
      record = glyph.raw;
    } else if (!glyph.components.empty()) {
      Box box = boxes[glyph.components[0].glyph];
      for (const Component& c : glyph.components) {
        const Box& b = boxes[c.glyph];
        box.xMin = std::min<int16_t>(box.xMin, static_cast<int16_t>(b.xMin + c.dx));
        box.yMin = std::min<int16_t>(box.yMin, static_cast<int16_t>(b.yMin + c.dy));
        box.xMax = std::max<int16_t>(box.xMax, static_cast<int16_t>(b.xMax + c.dx));
        box.yMax = std::max<int16_t>(box.yMax, static_cast<int16_t>(b.yMax + c.dy));
      }
      boxes[g] = box;
      record = encodeCompound(glyph, box);
    } else if (!glyph.contours.empty()) {
      record = encodeSimple(glyph);
    }
    glyf.insert(glyf.end(), record.begin(), record.end());
    while (glyf.size() % 4) glyf.push_back(0);
  }
  offsets.push_back(static_cast<uint32_t>(glyf.size()));
  Bytes loca;
  for (const uint32_t offset : offsets) {
    if (font.longLoca) {
      put32(loca, offset);
    } else {
      put16(loca, offset / 2);
    }
  }

  const uint16_t numGlyphs = static_cast<uint16_t>(font.glyphs.size());
  const uint16_t numHMetrics = font.numHMetrics ? font.numHMetrics : numGlyphs;
  Bytes hmtx;
  for (uint16_t g = 0; g < numGlyphs; ++g) {
    const Glyph& glyph = font.glyphs[g];
    const int16_t lsb = glyph.lsbIsXMin ? boxes[g].xMin : glyph.lsb;
    if (g < numHMetrics) put16(hmtx, glyph.advance);
    put16(hmtx, static_cast<uint16_t>(lsb));
  }

  // head's box: the union of the glyphs that have one.
  Box all;
  bool any = false;
  for (size_t g = 0; g < boxes.size(); ++g) {
    if (font.glyphs[g].contours.empty() && font.glyphs[g].components.empty()) continue;
    const Box& b = boxes[g];
    all = any ? Box{std::min(all.xMin, b.xMin), std::min(all.yMin, b.yMin), std::max(all.xMax, b.xMax),
                    std::max(all.yMax, b.yMax)}
              : b;
    any = true;
  }
  const int16_t xMin = all.xMin, yMin = all.yMin, xMax = all.xMax, yMax = all.yMax;
  Bytes head(54, 0);
  set32(head, 0, 0x00010000);
  set32(head, 12, 0x5F0F3CF5);
  set16(head, 18, font.unitsPerEm);
  set16(head, 36, static_cast<uint16_t>(xMin));
  set16(head, 38, static_cast<uint16_t>(yMin));
  set16(head, 40, static_cast<uint16_t>(xMax));
  set16(head, 42, static_cast<uint16_t>(yMax));
  set16(head, 44, font.macStyle);
  set16(head, 50, font.longLoca ? 1 : 0);

  Bytes hhea(36, 0);
  set32(hhea, 0, 0x00010000);
  set16(hhea, 4, static_cast<uint16_t>(font.ascender));
  set16(hhea, 6, static_cast<uint16_t>(font.descender));
  set16(hhea, 8, static_cast<uint16_t>(font.lineGap));
  set16(hhea, 34, numHMetrics);

  uint16_t maxPoints = 0, maxContours = 0;
  for (const Glyph& glyph : font.glyphs) {
    uint16_t points = 0;
    for (const auto& contour : glyph.contours) points = static_cast<uint16_t>(points + contour.size());
    maxPoints = std::max(maxPoints, points);
    maxContours = std::max(maxContours, static_cast<uint16_t>(glyph.contours.size()));
  }
  Bytes maxp(32, 0);
  set32(maxp, 0, 0x00010000);
  set16(maxp, 4, numGlyphs);
  set16(maxp, 6, maxPoints);
  set16(maxp, 8, maxContours);
  set16(maxp, 10, maxPoints);  // maxCompositePoints: a generous stand-in
  set16(maxp, 12, maxContours);

  std::vector<std::pair<std::string, Bytes>> tables = {
      {"head", head},
      {"hhea", hhea},
      {"maxp", maxp},
      {"cmap", font.cmapFormat12 ? cmapTable({{3, 10, cmapFormat12(font.cmap)}})
                                 : cmapTable({{3, 1, cmapFormat4(font.cmap, font.cmapRangeOffsets)}})},
      {"loca", loca},
      {"glyf", glyf},
      {"hmtx", hmtx},
  };
  if (font.os2) {
    Bytes os2(78, 0);
    set16(os2, 4, font.weightClass);
    set16(os2, 62, font.os2Selection);
    set16(os2, 68, static_cast<uint16_t>(font.typoAscender));
    set16(os2, 70, static_cast<uint16_t>(font.typoDescender));
    set16(os2, 72, static_cast<uint16_t>(font.typoLineGap));
    set16(os2, 74, font.winAscent);
    set16(os2, 76, font.winDescent);
    tables.emplace_back("OS/2", os2);
  }
  for (const auto& [name, data] : font.extraTables) {
    const auto it = std::find_if(tables.begin(), tables.end(), [&](const auto& t) { return t.first == name; });
    if (it != tables.end()) {
      it->second = data;
    } else {
      tables.emplace_back(name, data);
    }
  }
  std::sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) { return tagOf(a.first) < tagOf(b.first); });

  Bytes out;
  put32(out, font.sfntVersion);
  put16(out, static_cast<uint16_t>(tables.size()));
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  uint32_t offset = 12 + 16 * static_cast<uint32_t>(tables.size());
  for (const auto& [name, data] : tables) {
    put32(out, tagOf(name));
    put32(out, 0);
    put32(out, offset);
    put32(out, static_cast<uint32_t>(data.size()));
    offset += (static_cast<uint32_t>(data.size()) + 3) & ~3u;
  }
  for (const auto& entry : tables) {
    const Bytes& data = entry.second;
    out.insert(out.end(), data.begin(), data.end());
    while (out.size() % 4) out.push_back(0);
  }
  return out;
}

/// Where the table directory's record for `name` starts in `font` (0 when it has none).
inline size_t findRecord(const Bytes& font, const std::string& name) {
  const uint32_t tables = static_cast<uint32_t>(font[4] << 8 | font[5]);
  for (uint32_t i = 0; i < tables; ++i) {
    const size_t at = 12 + 16 * i;
    if (font[at] == name[0] && font[at + 1] == name[1] && font[at + 2] == name[2] && font[at + 3] == name[3]) return at;
  }
  return 0;
}

/// A rectangle in font units, counter-clockwise from the bottom left (TrueType's outer contours run
/// clockwise, but the nonzero rule fills either way).
inline std::vector<Point> rect(const int16_t x0, const int16_t y0, const int16_t x1, const int16_t y1) {
  return {{x0, y0, true}, {x1, y0, true}, {x1, y1, true}, {x0, y1, true}};
}

}  // namespace ttf_test

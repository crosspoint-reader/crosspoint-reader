#pragma once
// Builds the OpenType layout tables the kerning and ligature tests need (GPOS pair adjustment, GSUB ligatures, the
// legacy kern table), to go into a built font through Font::extraTables.
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "TtfBuilder.h"

namespace ttf_test {

/// A PairPos subtable. Format 1 lists pairs per left glyph; format 2 kerns classes (`class1` and `class2` give a
/// glyph's class, `values` is class1Count x class2Count). Left glyphs must be listed in glyph order.
struct PairSubtable {
  uint16_t format = 1;
  std::vector<std::pair<uint16_t, std::vector<std::pair<uint16_t, int16_t>>>> pairs;  // format 1
  std::vector<uint16_t> coverage;                                                     // format 2: left glyphs
  std::vector<std::pair<uint16_t, uint16_t>> class1;                                  // format 2: glyph, class
  std::vector<std::pair<uint16_t, uint16_t>> class2;
  uint16_t class1Count = 0;
  uint16_t class2Count = 0;
  std::vector<int16_t> values;
};

struct LayoutLookup {
  uint16_t type = 2;       // 2 pair adjustment (GPOS), 4 ligature (GSUB), or any other to be ignored
  bool extension = false;  // wrapped in an Extension subtable (GPOS type 9, GSUB type 7)
  std::vector<PairSubtable> pairSubtables;
  std::vector<std::pair<std::vector<uint16_t>, uint16_t>> ligatures;  // GSUB: components (first included), glyph
};

struct LayoutFeature {
  std::string tag;
  std::vector<uint16_t> lookups;
};

struct LayoutScript {
  std::string tag;
  std::vector<uint16_t> features;  // the default language system's
};

namespace detail {

inline void putTag(Bytes& out, const std::string& tag) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
}

inline Bytes coverage(std::vector<uint16_t> glyphs) {
  Bytes out;
  put16(out, 1);
  put16(out, static_cast<uint32_t>(glyphs.size()));
  for (const uint16_t g : glyphs) put16(out, g);
  return out;
}

// A ClassDef in format 2, one range per glyph.
inline Bytes classDef(std::vector<std::pair<uint16_t, uint16_t>> classes) {
  std::sort(classes.begin(), classes.end());
  Bytes out;
  put16(out, 2);
  put16(out, static_cast<uint32_t>(classes.size()));
  for (const auto& [glyph, cls] : classes) {
    put16(out, glyph);
    put16(out, glyph);
    put16(out, cls);
  }
  return out;
}

inline Bytes pairSubtable(const PairSubtable& t) {
  Bytes out;
  if (t.format == 1) {
    std::vector<uint16_t> lefts;
    for (const auto& p : t.pairs) lefts.push_back(p.first);
    const uint32_t header = 10 + 2 * static_cast<uint32_t>(t.pairs.size());
    put16(out, 1);
    put16(out, 0);  // coverage, set below
    put16(out, 0x0004);
    put16(out, 0);
    put16(out, static_cast<uint32_t>(t.pairs.size()));
    for (size_t i = 0; i < t.pairs.size(); ++i) put16(out, 0);
    out.resize(header);
    for (size_t i = 0; i < t.pairs.size(); ++i) {
      set16(out, 10 + 2 * i, static_cast<uint32_t>(out.size()));
      put16(out, static_cast<uint32_t>(t.pairs[i].second.size()));
      for (const auto& [second, value] : t.pairs[i].second) {
        put16(out, second);
        put16(out, static_cast<uint16_t>(value));
      }
    }
    set16(out, 2, static_cast<uint32_t>(out.size()));
    const Bytes cov = coverage(lefts);
    out.insert(out.end(), cov.begin(), cov.end());
    return out;
  }
  put16(out, 2);
  put16(out, 0);  // coverage
  put16(out, 0x0004);
  put16(out, 0);
  put16(out, 0);  // classDef1
  put16(out, 0);  // classDef2
  put16(out, t.class1Count);
  put16(out, t.class2Count);
  for (const int16_t v : t.values) put16(out, static_cast<uint16_t>(v));
  set16(out, 2, static_cast<uint32_t>(out.size()));
  const Bytes cov = coverage(t.coverage);
  out.insert(out.end(), cov.begin(), cov.end());
  set16(out, 8, static_cast<uint32_t>(out.size()));
  const Bytes c1 = classDef(t.class1);
  out.insert(out.end(), c1.begin(), c1.end());
  set16(out, 10, static_cast<uint32_t>(out.size()));
  const Bytes c2 = classDef(t.class2);
  out.insert(out.end(), c2.begin(), c2.end());
  return out;
}

// LigatureSubstFormat1 for ligatures that all start with the same glyph.
inline Bytes ligatureSubtable(const std::vector<std::pair<std::vector<uint16_t>, uint16_t>>& ligatures) {
  Bytes out;
  put16(out, 1);
  put16(out, 0);  // coverage
  put16(out, 1);  // one ligature set
  put16(out, 8);  // at 8
  put16(out, static_cast<uint32_t>(ligatures.size()));
  const size_t offsets = out.size();
  for (size_t i = 0; i < ligatures.size(); ++i) put16(out, 0);
  for (size_t i = 0; i < ligatures.size(); ++i) {
    set16(out, offsets + 2 * i, static_cast<uint32_t>(out.size() - 8));
    put16(out, ligatures[i].second);
    put16(out, static_cast<uint32_t>(ligatures[i].first.size()));
    for (size_t c = 1; c < ligatures[i].first.size(); ++c) put16(out, ligatures[i].first[c]);
  }
  set16(out, 2, static_cast<uint32_t>(out.size()));
  const Bytes cov = coverage({ligatures.front().first.front()});
  out.insert(out.end(), cov.begin(), cov.end());
  return out;
}

}  // namespace detail

/// A GPOS or GSUB table: its scripts (each with a default language system), features and lookups.
inline Bytes layoutTable(const std::vector<LayoutScript>& scripts, const std::vector<LayoutFeature>& features,
                         const std::vector<LayoutLookup>& lookups, const bool gsub = false) {
  Bytes scriptList;
  put16(scriptList, static_cast<uint32_t>(scripts.size()));
  for (size_t i = 0; i < scripts.size(); ++i) {
    detail::putTag(scriptList, scripts[i].tag);
    put16(scriptList, 0);
  }
  for (size_t i = 0; i < scripts.size(); ++i) {
    set16(scriptList, 2 + 6 * i + 4, static_cast<uint32_t>(scriptList.size()));
    put16(scriptList, 4);  // default language system right after this header
    put16(scriptList, 0);
    put16(scriptList, 0);
    put16(scriptList, 0xFFFF);
    put16(scriptList, static_cast<uint32_t>(scripts[i].features.size()));
    for (const uint16_t f : scripts[i].features) put16(scriptList, f);
  }
  Bytes featureList;
  put16(featureList, static_cast<uint32_t>(features.size()));
  for (size_t i = 0; i < features.size(); ++i) {
    detail::putTag(featureList, features[i].tag);
    put16(featureList, 0);
  }
  for (size_t i = 0; i < features.size(); ++i) {
    set16(featureList, 2 + 6 * i + 4, static_cast<uint32_t>(featureList.size()));
    put16(featureList, 0);
    put16(featureList, static_cast<uint32_t>(features[i].lookups.size()));
    for (const uint16_t l : features[i].lookups) put16(featureList, l);
  }
  Bytes lookupList;
  put16(lookupList, static_cast<uint32_t>(lookups.size()));
  for (size_t i = 0; i < lookups.size(); ++i) put16(lookupList, 0);
  for (size_t i = 0; i < lookups.size(); ++i) {
    const LayoutLookup& lookup = lookups[i];
    set16(lookupList, 2 + 2 * i, static_cast<uint32_t>(lookupList.size()));
    const size_t at = lookupList.size();
    std::vector<Bytes> subtables;
    if (lookup.type == 4 && !lookup.ligatures.empty()) subtables.push_back(detail::ligatureSubtable(lookup.ligatures));
    for (const auto& p : lookup.pairSubtables) subtables.push_back(detail::pairSubtable(p));
    put16(lookupList, lookup.extension ? (gsub ? 7 : 9) : lookup.type);
    put16(lookupList, 0);
    put16(lookupList, static_cast<uint32_t>(subtables.size()));
    for (size_t s = 0; s < subtables.size(); ++s) put16(lookupList, 0);
    for (size_t s = 0; s < subtables.size(); ++s) {
      set16(lookupList, at + 6 + 2 * s, static_cast<uint32_t>(lookupList.size() - at));
      if (lookup.extension) {
        put16(lookupList, 1);
        put16(lookupList, lookup.type);
        put32(lookupList, 8);
      }
      lookupList.insert(lookupList.end(), subtables[s].begin(), subtables[s].end());
    }
  }
  Bytes out;
  put32(out, 0x00010000);
  put16(out, 10);
  put16(out, static_cast<uint32_t>(10 + scriptList.size()));
  put16(out, static_cast<uint32_t>(10 + scriptList.size() + featureList.size()));
  out.insert(out.end(), scriptList.begin(), scriptList.end());
  out.insert(out.end(), featureList.begin(), featureList.end());
  out.insert(out.end(), lookupList.begin(), lookupList.end());
  return out;
}

/// The legacy kern table, version 0, one horizontal format 0 subtable.
inline Bytes legacyKernTable(std::vector<std::pair<std::pair<uint16_t, uint16_t>, int16_t>> pairs) {
  std::sort(pairs.begin(), pairs.end());
  Bytes out;
  put16(out, 0);
  put16(out, 1);
  put16(out, 0);
  put16(out, static_cast<uint32_t>(14 + 6 * pairs.size()));
  put16(out, 0x0001);
  put16(out, static_cast<uint32_t>(pairs.size()));
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  for (const auto& [glyphs, value] : pairs) {
    put16(out, glyphs.first);
    put16(out, glyphs.second);
    put16(out, static_cast<uint16_t>(value));
  }
  return out;
}

}  // namespace ttf_test

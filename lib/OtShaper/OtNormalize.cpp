#include <algorithm>

#include "OtShaperInternal.h"
#include "OtSort.h"
#include "OtUnicodeData.h"

// Character-level preparation before glyphs exist: Unicode properties,
// dotted circles between vowel sequences that look like another vowel
// (hb-ot-shaper-vowel-constraints.cc), and normalization
// (hb-ot-shape-normalize.cc). Passes that rebuild the string write into
// buffer.scratch and swap it in; appends past the reservation fail the run.

namespace ot {

namespace {

// Longest run of marks sorted by combining class (HB_OT_SHAPE_MAX_COMBINING_MARKS).
constexpr unsigned MAX_COMBINING_MARKS = 32;

// Vowel sequences that look like another vowel get a dotted circle between
// them. Pairs of (vowel, following sign); they apply within the run's script.
constexpr uint16_t VOWEL_CONSTRAINTS[][2] = {
    // Devanagari
    {0x0905, 0x093A},
    {0x0905, 0x093B},
    {0x0905, 0x093E},
    {0x0905, 0x0945},
    {0x0905, 0x0946},
    {0x0905, 0x0949},
    {0x0905, 0x094A},
    {0x0905, 0x094B},
    {0x0905, 0x094C},
    {0x0905, 0x094F},
    {0x0905, 0x0956},
    {0x0905, 0x0957},
    {0x0906, 0x093A},
    {0x0906, 0x0945},
    {0x0906, 0x0946},
    {0x0906, 0x0947},
    {0x0906, 0x0948},
    {0x0909, 0x0941},
    {0x090F, 0x0945},
    {0x090F, 0x0946},
    {0x090F, 0x0947},
    // Bengali
    {0x0985, 0x09BE},
    {0x098B, 0x09C3},
    {0x098C, 0x09E2},
    // Gurmukhi
    {0x0A05, 0x0A3E},
    {0x0A05, 0x0A48},
    {0x0A05, 0x0A4C},
    {0x0A72, 0x0A3F},
    {0x0A72, 0x0A40},
    {0x0A72, 0x0A47},
    {0x0A73, 0x0A41},
    {0x0A73, 0x0A42},
    {0x0A73, 0x0A4B},
    // Gujarati
    {0x0A85, 0x0ABE},
    {0x0A85, 0x0AC5},
    {0x0A85, 0x0AC7},
    {0x0A85, 0x0AC8},
    {0x0A85, 0x0AC9},
    {0x0A85, 0x0ACB},
    {0x0A85, 0x0ACC},
    {0x0AC5, 0x0ABE},
    // Oriya
    {0x0B05, 0x0B3E},
    {0x0B0F, 0x0B57},
    {0x0B13, 0x0B57},
    // Tamil
    {0x0B85, 0x0BC2},
    // Telugu
    {0x0C12, 0x0C4C},
    {0x0C12, 0x0C55},
    {0x0C3F, 0x0C55},
    {0x0C46, 0x0C55},
    {0x0C4A, 0x0C55},
    // Kannada
    {0x0C89, 0x0CBE},
    {0x0C8B, 0x0CBE},
    {0x0C92, 0x0CCC},
    // Malayalam
    {0x0D07, 0x0D57},
    {0x0D09, 0x0D57},
    {0x0D0E, 0x0D46},
    {0x0D12, 0x0D3E},
    {0x0D12, 0x0D57},
    // Sinhala
    {0x0D85, 0x0DCF},
    {0x0D85, 0x0DD0},
    {0x0D85, 0x0DD1},
    {0x0D8B, 0x0DDF},
    {0x0D8F, 0x0DDF},
    {0x0D94, 0x0DDF},
    {0x0D8D, 0x0DD8},
    {0x0D91, 0x0DCA},
    {0x0D91, 0x0DD9},
    {0x0D91, 0x0DDA},
    {0x0D91, 0x0DDC},
    {0x0D91, 0x0DDD},
    {0x0D91, 0x0DDE},
};

bool inScriptBlock(const uint32_t cp, const Script script) {
  const uint32_t first = 0x0900 + 0x80 * static_cast<uint32_t>(script);
  return cp >= first && cp < first + 0x80;
}

// Appends to the scratch string within the run's reservation.
bool pushScratch(Buffer& buffer, const GlyphInfo& g) {
  if (buffer.scratch.size() >= buffer.maxLength) {
    buffer.successful = false;
    return false;
  }
  buffer.scratch.push_back(g);
  return true;
}

// The normalizer, with the Indic and USE shapers' decompose/compose
// overrides (decompose_indic, compose_indic, compose_use).
class Normalizer {
 public:
  Normalizer(const Face& face, const Plan& plan, Buffer& buffer) : face_(face), plan_(plan), buffer_(buffer) {}

  void run() {
    const unsigned count = buffer_.len();
    if (!count) return;
    // The Indic shapers decompose everything (COMPOSED_DIACRITICS_NO_SHORT_
    // CIRCUIT); the default shaper keeps characters the font maps.
    const bool mightShortCircuit = plan_.shaper == ShaperKind::Default;
    const bool allSimple = decomposeAll(count, mightShortCircuit);
    if (!buffer_.successful || allSimple) return;
    reorderMarks();
    recompose();
  }

 private:
  bool decompose1(const uint32_t ab, uint32_t* a, uint32_t* b) const {
    // decompose_indic(): keep these composed (they are separate letters in
    // their scripts' fonts).
    if (plan_.shaper == ShaperKind::Indic && (ab == 0x0931 || ab == 0x09DC || ab == 0x09DD || ab == 0x0B94)) {
      return false;
    }
    for (const auto& d : ucd::DECOMPOSITIONS) {
      if (d.ab == ab) {
        *a = d.a;
        *b = d.b;
        return true;
      }
    }
    return false;
  }

  bool compose1(const uint32_t a, const uint32_t b, uint32_t* ab) const {
    // Split matras stay split: nothing composes onto a mark.
    if (plan_.shaper != ShaperKind::Default && gc::isMark(charData(a).genCat)) return false;
    // Bengali YYA is a composition exclusion the Indic shaper composes anyway.
    if (plan_.shaper == ShaperKind::Indic && a == 0x09AF && b == 0x09BC) {
      *ab = 0x09DF;
      return true;
    }
    for (const auto& c : ucd::COMPOSITIONS) {
      if (c.a == a && c.b == b) {
        *ab = c.ab;
        return true;
      }
    }
    return false;
  }

  bool outputChar(const GlyphInfo& from, const uint32_t unichar, const uint32_t glyph) {
    GlyphInfo g = from;
    g.codepoint = unichar;
    g.glyphIndex = static_cast<uint16_t>(glyph);
    setUnicodeProps(g, buffer_);
    return pushScratch(buffer_, g);
  }

  // decompose(): the number of characters `ab` was output as, 0 when it
  // does not decompose into characters the font maps.
  unsigned decompose(const GlyphInfo& cur, const bool shortest, const uint32_t ab) {
    uint32_t a = 0, b = 0, aGlyph = 0, bGlyph = 0;
    if (!decompose1(ab, &a, &b) || (b && !face_.nominalGlyph(b, &bGlyph))) return 0;
    const bool hasA = face_.nominalGlyph(a, &aGlyph);
    unsigned ret = 0;
    if (!(shortest && hasA)) ret = decompose(cur, shortest, a);
    if (!ret) {
      if (!hasA) return 0;
      outputChar(cur, a, aGlyph);
      ret = 1;
    }
    if (b) {
      outputChar(cur, b, bGlyph);
      ret++;
    }
    return ret;
  }

  void nextChar(const uint32_t glyph) {
    GlyphInfo g = buffer_.info[buffer_.idx++];
    g.glyphIndex = static_cast<uint16_t>(glyph);
    pushScratch(buffer_, g);
  }

  void decomposeCurrentCharacter(const bool shortest) {
    const GlyphInfo cur = buffer_.info[buffer_.idx];
    uint32_t glyph = 0;
    if (shortest && face_.nominalGlyph(cur.codepoint, &glyph)) {
      nextChar(glyph);
    } else if (decompose(cur, shortest, cur.codepoint)) {
      buffer_.idx++;  // the character itself is dropped
    } else if (!shortest && face_.nominalGlyph(cur.codepoint, &glyph)) {
      nextChar(glyph);
    } else {
      nextChar(0);
    }
  }

  // First round: decompose. Returns true when the text has no marks, so
  // nothing needs reordering or recomposing.
  bool decomposeAll(const unsigned count, const bool mightShortCircuit) {
    buffer_.scratch.clear();
    bool allSimple = true;
    buffer_.idx = 0;
    while (buffer_.idx < count && buffer_.successful) {
      unsigned end;
      for (end = buffer_.idx + 1; end < count; end++) {
        if (buffer_.info[end].isUnicodeMark()) break;
      }
      if (end < count) end--;  // leave one base for the marks to cluster with
      if (mightShortCircuit) {
        uint32_t glyph;
        while (buffer_.idx < end && face_.nominalGlyph(buffer_.info[buffer_.idx].codepoint, &glyph)) nextChar(glyph);
      }
      while (buffer_.idx < end) decomposeCurrentCharacter(mightShortCircuit);
      if (buffer_.idx == count) break;
      allSimple = false;
      // A base and its marks always decompose fully.
      for (end = buffer_.idx + 1; end < count; end++) {
        if (!buffer_.info[end].isUnicodeMark()) break;
      }
      while (buffer_.idx < end) decomposeCurrentCharacter(false);
    }
    buffer_.info.swap(buffer_.scratch);
    return allSimple;
  }

  // Second round: marks in combining-class order (a stable sort).
  void reorderMarks() {
    std::vector<GlyphInfo>& info = buffer_.info;
    const unsigned len = buffer_.len();
    for (unsigned i = 0; i < len; i++) {
      if (info[i].combiningClass() == 0) continue;
      unsigned end;
      for (end = i + 1; end < len; end++) {
        if (info[end].combiningClass() == 0) break;
      }
      if (end - i <= MAX_COMBINING_MARKS) {
        stableSort(info.data() + i, info.data() + end,
                   [](const GlyphInfo& a, const GlyphInfo& b) { return a.combiningClass() < b.combiningClass(); });
      }
      i = end;
    }
  }

  // Third round: recompose marks onto their starter when nothing between
  // them blocks it and the font maps the composition.
  void recompose() {
    const std::vector<GlyphInfo>& info = buffer_.info;
    std::vector<GlyphInfo>& out = buffer_.scratch;
    out.clear();
    unsigned starter = 0;
    out.push_back(info[0]);
    for (unsigned i = 1; i < buffer_.len(); i++) {
      const GlyphInfo& cur = info[i];
      uint32_t composed, glyph;
      if (cur.isUnicodeMark() && (starter == out.size() - 1 || out.back().combiningClass() < cur.combiningClass()) &&
          compose1(out[starter].codepoint, cur.codepoint, &composed) && face_.nominalGlyph(composed, &glyph)) {
        GlyphInfo& s = out[starter];
        s.codepoint = composed;
        s.glyphIndex = static_cast<uint16_t>(glyph);
        setUnicodeProps(s, buffer_);
        continue;
      }
      out.push_back(cur);  // recomposing only shrinks the string
      if (out.back().combiningClass() == 0) starter = static_cast<unsigned>(out.size() - 1);
    }
    buffer_.info.swap(out);
  }

  const Face& face_;
  const Plan& plan_;
  Buffer& buffer_;
};

}  // namespace

CharData charData(const uint32_t cp) {
  uint32_t packed;
  if (cp >= ucd::FIRST && cp <= ucd::LAST) {
    packed = ucd::PROPS[ucd::PROPS_INDEX[cp - ucd::FIRST]];
  } else if (cp == 0x200C) {
    packed = ucd::ZWNJ_PROPS;
  } else if (cp == 0x200D) {
    packed = ucd::ZWJ_PROPS;
  } else if (cp == DOTTED_CIRCLE) {
    packed = ucd::DOTTED_CIRCLE_PROPS;
  } else {
    return CharData{gc::UNASSIGNED, 0, 0, 0, 0, false};
  }
  const auto field = [packed](const unsigned shift, const unsigned bits) {
    return static_cast<uint8_t>((packed >> shift) & ((1u << bits) - 1));
  };
  return CharData{field(ucd::GEN_CAT_SHIFT, ucd::GEN_CAT_BITS),     field(ucd::CCC_SHIFT, ucd::CCC_BITS),
                  field(ucd::INDIC_CAT_SHIFT, ucd::INDIC_CAT_BITS), field(ucd::INDIC_POS_SHIFT, ucd::INDIC_POS_BITS),
                  field(ucd::USE_CAT_SHIFT, ucd::USE_CAT_BITS),     ((packed >> ucd::IGNORABLE_SHIFT) & 1) != 0};
}

void setUnicodeProps(GlyphInfo& info, Buffer& buffer) {
  const uint32_t u = info.codepoint;
  const CharData d = charData(u);
  uint16_t p = d.genCat;
  if (u >= 0x80) {
    if (d.ignorable) {
      buffer.hasDefaultIgnorables = true;
      p |= uprops::IGNORABLE;
      if (u == 0x200C) {
        p |= uprops::CF_ZWNJ;
      } else if (u == 0x200D) {
        p |= uprops::CF_ZWJ;
      }
    }
    if (gc::isMark(d.genCat)) p |= uprops::CONTINUATION | static_cast<uint16_t>(d.ccc << 8);
  }
  info.unicodeProps = p;
}

void preprocessVowelConstraints(const Script script, Buffer& buffer) {
  const unsigned count = buffer.len();
  if (count < 2) return;
  buffer.scratch.clear();
  bool inserted = false;
  unsigned i = 0;
  while (i + 1 < count && buffer.successful) {
    const uint32_t cur = buffer.info[i].codepoint;
    const uint32_t next = buffer.info[i + 1].codepoint;
    bool matched = false;
    if (inScriptBlock(cur, script)) {
      for (const auto& pair : VOWEL_CONSTRAINTS) {
        if (pair[0] == cur && pair[1] == next) {
          matched = true;
          break;
        }
      }
      // Devanagari RA, VIRAMA before independent I.
      if (script == Script::Devanagari && cur == 0x0930 && next == 0x094D && i + 2 < count &&
          buffer.info[i + 2].codepoint == 0x0907) {
        pushScratch(buffer, buffer.info[i++]);
        matched = true;
      }
    }
    pushScratch(buffer, buffer.info[i++]);
    if (matched) {
      // The circle copies the sign it precedes (as HarfBuzz's output_glyph()).
      GlyphInfo dc = buffer.info[i];
      dc.codepoint = DOTTED_CIRCLE;
      dc.unicodeProps &= ~uprops::CONTINUATION;
      pushScratch(buffer, dc);
      pushScratch(buffer, buffer.info[i++]);
      inserted = true;
    }
  }
  if (!inserted || !buffer.successful) return;
  while (i < count) pushScratch(buffer, buffer.info[i++]);
  buffer.info.swap(buffer.scratch);
}

void normalize(const Face& face, const Plan& plan, Buffer& buffer) { Normalizer(face, plan, buffer).run(); }

}  // namespace ot

#include "TtfOpenType.h"

#include <Gpos.h>
#include <Gsub.h>
#include <KernTable.h>
#include <Memory.h>

namespace ttf {

bool Kerning::resolve(FontBytes& bytes, const Face& face) {
  subtables_.reset();
  count_ = 0;
  legacy_ = false;
  const uint8_t* font = bytes.data();
  if (font == nullptr) return false;
  if (face.gpos != 0) {
    const uint8_t* gpos = font + face.gpos;
    const unsigned found = freeink::font::gpos::ResolveKernSubtables(gpos, face.gposLength, nullptr, 0);
    if (found != 0) {
      const uint16_t keep = found < kMaxSubtables ? static_cast<uint16_t>(found) : kMaxSubtables;
      subtables_ = makeUniqueNoThrow<uint32_t[]>(keep);
      if (!subtables_) return false;
      freeink::font::gpos::ResolveKernSubtables(gpos, face.gposLength, subtables_.get(), keep);
      count_ = keep;
      return true;
    }
  }
  // No GPOS kerning: the legacy table, read in place at each pair.
  legacy_ = face.kern != 0;
  return legacy_;
}

int32_t Kerning::pair(FontBytes& bytes, const Face& face, const uint16_t left, const uint16_t right,
                      bool* covered) const {
  const uint8_t* font = bytes.data();
  if (font != nullptr && count_ != 0) {
    return freeink::font::gpos::PairKernAdjustment(font + face.gpos, face.gposLength, subtables_.get(), count_, left,
                                                   right, covered);
  }
  if (font != nullptr && legacy_) {
    return freeink::font::kerntable::PairKernAdjustment(font + face.kern, face.kernLength, left, right, covered);
  }
  if (covered) *covered = false;
  return 0;
}

uint16_t Ligatures::glyphFor(FontBytes& bytes, const Face& face, const uint32_t codepoint) const {
  if (codepoint >= kFirst && codepoint < kFirst + 5 && glyphs[codepoint - kFirst] != 0) {
    return glyphs[codepoint - kFirst];
  }
  return glyphIndex(bytes, face, codepoint);
}

bool resolveLigatures(FontBytes& bytes, const Face& face, Ligatures* out) {
  *out = Ligatures{};
  if (face.gsub == 0 || bytes.data() == nullptr) return false;
  const uint32_t f = glyphIndex(bytes, face, 'f');
  const uint32_t i = glyphIndex(bytes, face, 'i');
  const uint32_t l = glyphIndex(bytes, face, 'l');
  if (f == 0) return false;
  const uint32_t sequences[5][3] = {{f, f, 0}, {f, i, 0}, {f, l, 0}, {f, f, i}, {f, f, l}};
  const unsigned lengths[5] = {2, 2, 2, 3, 3};
  for (uint8_t k = 0; k < 5; ++k) {
    if (sequences[k][lengths[k] - 1] == 0) continue;
    const uint32_t glyph =
        freeink::font::gsub::LigatureGlyphId(bytes.data() + face.gsub, face.gsubLength, sequences[k], lengths[k]);
    // A glyph the font does not have would draw as missing, every time: without it the letters draw instead.
    out->glyphs[k] = glyph != 0 && glyph < face.numGlyphs ? static_cast<uint16_t>(glyph) : 0;
  }
  // In the order EpdFont searches them (left << 16 | right); ffi and ffl chain through ff.
  const auto add = [out](const uint32_t left, const uint32_t right, const uint8_t k) {
    if (out->glyphs[k] != 0) out->pairs[out->count++] = EpdLigaturePair{left << 16 | right, Ligatures::kFirst + k};
  };
  add('f', 'f', 0);
  add('f', 'i', 1);
  add('f', 'l', 2);
  if (out->glyphs[0] != 0) {
    add(Ligatures::kFirst, 'i', 3);
    add(Ligatures::kFirst, 'l', 4);
  }
  return out->count != 0;
}

}  // namespace ttf

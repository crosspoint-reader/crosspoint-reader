#include "OtShaper.h"

#include <algorithm>
#include <cstring>

#include "OtShaperInternal.h"
#include "OtUnicodeData.h"

// The shaping pipeline (HarfBuzz's hb_ot_shape_internal): Unicode
// properties, vowel constraints, normalization, character categories, GSUB
// stages with the shaper's pauses, then positioning and hiding of
// default-ignorable characters.

namespace ot {

namespace {

// Most glyphs a run may become while shaping, relative to its length.
// Real text grows by a few glyphs at most (split vowels, dotted circles);
// the cap stops pathological fonts and bounds the buffer's reservation.
constexpr unsigned MAX_LENGTH_FACTOR = 2;
constexpr unsigned MAX_LENGTH_EXTRA = 16;
// Nested-lookup budget (HarfBuzz's max_ops), relative to the run length.
constexpr int MAX_OPS_FACTOR = 64;
constexpr int MAX_OPS_MIN = 1024;

void runPause(const Pause pause, const Face& face, const Plan& plan, Buffer& buffer) {
  switch (pause) {
    case Pause::None:
      break;
    case Pause::IndicSetupSyllables:
      indicSetupSyllables(buffer);
      break;
    case Pause::IndicInitialReordering:
      indicInitialReordering(face, plan, buffer);
      break;
    case Pause::IndicFinalReordering:
      indicFinalReordering(face, plan, buffer);
      break;
    case Pause::UseSetupSyllables:
      useSetupSyllables(plan, buffer);
      break;
    case Pause::UseClearSubstitutionFlags:
      for (GlyphInfo& g : buffer.info) g.glyphProps &= ~props::SUBSTITUTED;
      break;
    case Pause::UseRecordRphf:
      useRecordRphf(plan, buffer);
      break;
    case Pause::UseRecordPref:
      useRecordPref(buffer);
      break;
    case Pause::UseReorder:
      useReorder(face, buffer);
      break;
  }
}

// hb_ot_map_t::apply(): every stage's lookups, then its pause.
void applyTable(const Face& face, const Scale& scale, const Plan& plan, Buffer& buffer, const int table) {
  size_t i = 0;
  for (const Stage& stage : plan.stages[table]) {
    for (; i < stage.lastLookup; i++) {
      const LookupSettings settings = plan.settings(table, i);
      // Only lookups that can start at a glyph the buffer holds.
      if (settings.hasDigest && !settings.digest.mayIntersect(buffer.digest)) continue;
      applyLookup(face, scale, buffer, table, plan.lookups[table][i].index, settings);
      if (!buffer.successful) return;
    }
    if (stage.pause == Pause::None) continue;
    runPause(stage.pause, face, plan, buffer);
    if (!buffer.successful) return;
    buffer.refreshDigest();
  }
}

void zeroMarkWidths(Buffer& buffer, const bool adjustOffsets) {
  for (unsigned i = 0; i < buffer.len(); i++) {
    if (!buffer.info[i].isMark()) continue;
    GlyphPosition& p = buffer.pos[i];
    if (adjustOffsets) {
      p.xOffset -= p.xAdvance;
      p.yOffset -= p.yAdvance;
    }
    p.xAdvance = 0;
    p.yAdvance = 0;
  }
}

void position(const Face& face, const Scale& scale, const Plan& plan, Buffer& buffer) {
  const unsigned len = buffer.len();
  buffer.pos.assign(len, GlyphPosition{});
  for (unsigned i = 0; i < len; i++) buffer.pos[i].xAdvance = scale.emScaleX(face.advance(buffer.info[i].codepoint));
  // USE zeroes mark advances before GPOS, the default shaper after it.
  const bool adjustOffsets = plan.adjustMarkPositioningWhenZeroing;
  if (plan.zeroMarks && plan.shaper == ShaperKind::Use) zeroMarkWidths(buffer, adjustOffsets);
  if (plan.applyGpos) applyTable(face, scale, plan, buffer, GPOS);
  if (plan.zeroMarks && plan.shaper == ShaperKind::Default) zeroMarkWidths(buffer, adjustOffsets);
  if (buffer.hasDefaultIgnorables) {
    for (unsigned i = 0; i < len; i++) {
      if (buffer.info[i].isDefaultIgnorable()) {
        buffer.pos[i].xAdvance = buffer.pos[i].yAdvance = 0;
        buffer.pos[i].xOffset = 0;
      }
    }
  }
  propagateAttachmentOffsets(buffer);
}

// hb_ot_hide_default_ignorables(): joiners become an invisible space (or go,
// in a font without one).
void hideDefaultIgnorables(const Face& face, Buffer& buffer) {
  if (!buffer.hasDefaultIgnorables) return;
  uint32_t space;
  if (face.nominalGlyph(' ', &space)) {
    for (GlyphInfo& g : buffer.info) {
      if (g.isDefaultIgnorable()) g.codepoint = space;
    }
    return;
  }
  unsigned j = 0;
  for (unsigned i = 0; i < buffer.len(); i++) {
    if (buffer.info[i].isDefaultIgnorable()) continue;
    buffer.info[j] = buffer.info[i];
    buffer.pos[j] = buffer.pos[i];
    j++;
  }
  buffer.info.resize(j);
  buffer.pos.resize(j);
}

}  // namespace

void insertDottedCircles(const Face& face, Buffer& buffer, const uint8_t brokenSyllableType, const uint8_t category,
                         const uint8_t rephaCategory, const bool setPosition, const uint8_t position) {
  if (!buffer.hasBrokenSyllable) return;
  uint32_t glyph;
  if (!face.nominalGlyph(DOTTED_CIRCLE, &glyph)) return;
  buffer.scratch.clear();
  unsigned i = 0;
  unsigned lastSyllable = 0;
  const unsigned len = buffer.len();
  const auto push = [&buffer](const GlyphInfo& g) {
    if (buffer.scratch.size() >= buffer.maxLength) {
      buffer.successful = false;
      return;
    }
    buffer.scratch.push_back(g);
  };
  while (i < len && buffer.successful) {
    const unsigned syllable = buffer.info[i].syllable;
    if (lastSyllable == syllable || buffer.info[i].syllableType() != brokenSyllableType) {
      push(buffer.info[i++]);
      continue;
    }
    lastSyllable = syllable;
    GlyphInfo dc{};
    dc.codepoint = glyph;
    dc.category = category;
    if (setPosition) dc.position = position;
    dc.mask = buffer.info[i].mask;
    dc.syllable = buffer.info[i].syllable;
    while (i < len && lastSyllable == buffer.info[i].syllable && buffer.info[i].category == rephaCategory) {
      push(buffer.info[i++]);
    }
    push(dc);
  }
  if (buffer.successful) buffer.info.swap(buffer.scratch);
}

bool shape(const Face& face, const Scale& scale, const Plan& plan, const uint32_t* codepoints, const unsigned count,
           Buffer& buffer) {
  if (!buffer.prepare(count * MAX_LENGTH_FACTOR + MAX_LENGTH_EXTRA)) return false;
  if (!count) return true;
  buffer.maxOps = std::max(static_cast<int>(count) * MAX_OPS_FACTOR, MAX_OPS_MIN);

  // hb_set_unicode_props().
  for (unsigned i = 0; i < count; i++) {
    GlyphInfo g{};
    g.codepoint = codepoints[i];
    g.mask = plan.globalMask;
    setUnicodeProps(g, buffer);
    if (g.isZwj()) g.unicodeProps |= uprops::CONTINUATION;
    buffer.info.push_back(g);
  }

  if (plan.shaper != ShaperKind::Default) preprocessVowelConstraints(plan.script, buffer);
  normalize(face, plan, buffer);
  if (!buffer.successful) return false;

  if (plan.shaper == ShaperKind::Indic) {
    indicSetupMasks(buffer);
  } else if (plan.shaper == ShaperKind::Use) {
    useSetupMasks(buffer);
  }

  // Map to glyphs and set their properties (hb_ot_layout_substitute_start).
  for (GlyphInfo& g : buffer.info) {
    g.codepoint = g.glyphIndex;
    g.glyphProps = face.glyphProps(g.codepoint);
    g.ligProps = 0;
  }
  buffer.refreshDigest();
  if (plan.fallbackGlyphClasses) {
    for (GlyphInfo& g : buffer.info) {
      g.glyphProps = (g.genCat() != gc::NON_SPACING_MARK || g.isDefaultIgnorable()) ? props::BASE_GLYPH : props::MARK;
    }
  }

  applyTable(face, scale, plan, buffer, GSUB);
  if (!buffer.successful) return false;
  position(face, scale, plan, buffer);
  hideDefaultIgnorables(face, buffer);
  return buffer.successful;
}

const uint32_t* languageTagsFor(const char* bcp47) {
  static constexpr uint32_t NONE[1] = {0};
  if (!bcp47 || !*bcp47) return NONE;
  char primary[4] = {};
  for (size_t n = 0; bcp47[n] && bcp47[n] != '-' && bcp47[n] != '_'; n++) {
    if (n >= 3) return NONE;
    const char c = bcp47[n];
    primary[n] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  }
  for (const auto& l : ucd::LANGUAGES) {
    if (strcmp(l.code, primary) == 0) return l.tags;
  }
  return NONE;
}

}  // namespace ot

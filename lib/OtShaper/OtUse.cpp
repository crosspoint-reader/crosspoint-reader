#include <algorithm>
#include <vector>

#include "OtShaperInternal.h"
#include "OtSyllableMachines.h"

// The Universal Shaping Engine, as far as the Indic blocks need it: Sinhala
// always shapes with it, and so do fonts that tag an Indic script with the
// "3" script tags (dev3, bng3, ...). A port of HarfBuzz's hb-ot-shaper-use.cc
// and hb-ot-shaper-use-machine.rl.

namespace ot {

namespace {

bool isHalantUse(const GlyphInfo& info) {
  return (info.category == ucat::H || info.category == ucat::HVM || info.category == ucat::IS) && !info.ligated();
}

constexpr uint64_t POST_BASE_FLAGS =
    flag(ucat::FAbv) | flag(ucat::FBlw) | flag(ucat::FPst) | flag(ucat::FMAbv) | flag(ucat::FMBlw) | flag(ucat::FMPst) |
    flag(ucat::MAbv) | flag(ucat::MBlw) | flag(ucat::MPst) | flag(ucat::MPre) | flag(ucat::VAbv) | flag(ucat::VBlw) |
    flag(ucat::VPst) | flag(ucat::VPre) | flag(ucat::VMAbv) | flag(ucat::VMBlw) | flag(ucat::VMPst) | flag(ucat::VMPre);

void reorderSyllable(Buffer& buffer, const unsigned start, const unsigned end) {
  const auto type = buffer.info[start].syllableType();
  if (type != usyl::VIRAMA_TERMINATED && type != usyl::SAKOT_TERMINATED && type != usyl::STANDARD &&
      type != usyl::SYMBOL && type != usyl::BROKEN) {
    return;
  }
  auto& info = buffer.info;

  // A repha moves towards the end, before the first post-base glyph.
  if (info[start].category == ucat::R && end - start > 1) {
    for (unsigned i = start + 1; i < end; i++) {
      const bool isPostBase = ((POST_BASE_FLAGS >> info[i].category) & 1) || isHalantUse(info[i]);
      if (isPostBase || i == end - 1) {
        if (isPostBase) i--;
        buffer.moveGlyph(start, i);
        break;
      }
    }
  }

  // Pre-base vowels move to the start, or after the last halant.
  unsigned j = start;
  for (unsigned i = start; i < end; i++) {
    const uint8_t c = info[i].category;
    if (isHalantUse(info[i])) {
      j = i + 1;
    } else if ((c == ucat::VPre || c == ucat::VMPre) && info[i].ligComp() == 0 && j < i) {
      buffer.moveGlyph(i, j);
    }
  }
}

}  // namespace

void useSetupMasks(Buffer& buffer) {
  for (GlyphInfo& g : buffer.info) g.category = charData(g.codepoint).useCategory;
}

void useSetupSyllables(const Plan& plan, Buffer& buffer) {
  const unsigned len = buffer.len();
  // The machine skips CGJ-category characters (ZWJ among them), and a ZWNJ
  // that a mark follows.
  std::vector<uint16_t>& index = buffer.indices;
  index.clear();
  for (unsigned i = 0; i < len; i++) {
    const GlyphInfo& g = buffer.info[i];
    if (g.category == ucat::CGJ) continue;
    if (g.category == ucat::ZWNJ) {
      bool keep = true;
      for (unsigned k = i + 1; k < len; k++) {
        if (buffer.info[k].category != ucat::CGJ) {
          keep = !buffer.info[k].isUnicodeMark();
          break;
        }
      }
      if (!keep) continue;
    }
    index.push_back(i);
  }
  for (GlyphInfo& g : buffer.info) g.syllable = 0;

  using namespace machines;
  unsigned serial = 1;
  const auto count = static_cast<unsigned>(index.size());
  unsigned p = 0;
  while (p < count) {
    uint8_t found = usyl::NON_CLUSTER;
    unsigned length = scanSyllable(
        USE_CLASSES, USE_CLASS_COUNT, USE_TRANSITIONS, USE_ACCEPT, count - p,
        [&](const unsigned i) { return buffer.info[index[p + i]].category; }, &found);
    if (length == 0) length = 1;
    const auto type = found;
    if (type == usyl::BROKEN) buffer.hasBrokenSyllable = true;
    const unsigned from = index[p];
    const unsigned to = p + length < count ? index[p + length] : len;
    for (unsigned i = from; i < to; i++) buffer.info[i].syllable = static_cast<uint8_t>((serial << 4) | type);
    serial++;
    if (serial == 16) serial = 1;
    p += length;
  }

  // setup_rphf_mask(): the first glyphs of each syllable may form repha.
  if (plan.useRphfMask) {
    forEachSyllable(buffer, [&](const unsigned start, const unsigned end) {
      const unsigned limit = buffer.info[start].category == ucat::R ? 1 : std::min(3u, end - start);
      for (unsigned i = start; i < start + limit; i++) buffer.info[i].mask |= plan.useRphfMask;
    });
  }

  // setup_topographical_masks(): isol/init/medi/fina by syllable joining.
  uint32_t allMasks = 0;
  for (const uint32_t m : plan.useTopographicalMasks) allMasks |= m;
  if (!allMasks) return;
  const uint32_t otherMasks = ~allMasks;
  enum { ISOL, INIT, MEDI, FINA, NONE };
  unsigned lastStart = 0;
  int lastForm = NONE;
  forEachSyllable(buffer, [&](const unsigned start, const unsigned end) {
    const auto type = buffer.info[start].syllableType();
    if (type == usyl::HIEROGLYPH || type == usyl::NON_CLUSTER) {
      lastForm = NONE;
    } else {
      const bool join = lastForm == FINA || lastForm == ISOL;
      if (join) {
        lastForm = lastForm == FINA ? MEDI : INIT;
        for (unsigned i = lastStart; i < start; i++) {
          buffer.info[i].mask = (buffer.info[i].mask & otherMasks) | plan.useTopographicalMasks[lastForm];
        }
      }
      lastForm = join ? FINA : ISOL;
      for (unsigned i = start; i < end; i++) {
        buffer.info[i].mask = (buffer.info[i].mask & otherMasks) | plan.useTopographicalMasks[lastForm];
      }
    }
    lastStart = start;
  });
}

void useRecordRphf(const Plan& plan, Buffer& buffer) {
  const uint32_t mask = plan.useRphfMask;
  if (!mask) return;
  forEachSyllable(buffer, [&](const unsigned start, const unsigned end) {
    for (unsigned i = start; i < end && (buffer.info[i].mask & mask); i++) {
      if (buffer.info[i].substituted()) {
        buffer.info[i].category = ucat::R;
        break;
      }
    }
  });
}

void useRecordPref(Buffer& buffer) {
  forEachSyllable(buffer, [&](const unsigned start, const unsigned end) {
    for (unsigned i = start; i < end; i++) {
      if (buffer.info[i].substituted()) {
        buffer.info[i].category = ucat::VPre;
        break;
      }
    }
  });
}

void useReorder(const Face& face, Buffer& buffer) {
  insertDottedCircles(face, buffer, usyl::BROKEN, ucat::B, ucat::R, false, 0);
  if (!buffer.successful) return;
  forEachSyllable(buffer, [&](const unsigned start, const unsigned end) { reorderSyllable(buffer, start, end); });
}

}  // namespace ot

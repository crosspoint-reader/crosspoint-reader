#include "EpdFont.h"

#include <Utf8.h>

#include <algorithm>

#include "GlyphFallback.h"
#include "GlyphProvider.h"
#include "SmallCaps.h"

// Scale a 12.4 fixed-point advance by the small-caps factor, rounding to nearest.
static inline int32_t scaleAdvanceFP(const int32_t advanceFP) {
  return static_cast<int32_t>(advanceFP * smallCaps::SCALE + 0.5f);
}

void EpdFont::getTextBounds(const char* string, const int startX, const int startY, int* minX, int* minY, int* maxX,
                            int* maxY, const bool useSmallCaps) const {
  *minX = startX;
  *minY = startY;
  *maxX = startX;
  *maxY = startY;

  if (*string == '\0') {
    return;
  }

  int lastBaseX = startX;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int lastBaseAdvanceFP = 0;  // 12.4 fixed-point
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const bool isCombining = utf8IsCombiningMark(cp);

    // No ligatures under small caps, as EpdFontFamily::applyLigatures() for the renderer.
    if (!isCombining && !useSmallCaps) {
      cp = applyLigatures(cp, string);
    }

    // Small-caps: fold lowercase to uppercase and mark this glyph for scaled metrics.
    const bool folded = useSmallCaps && !isCombining && smallCaps::fold(cp);

    const EpdGlyphRef glyph = getGlyph(cp);
    if (!glyph) {
      lastBaseX += fp4::toPixel(prevAdvanceFP);  // flush pending advance before resetting
      prevCp = 0;
      prevAdvanceFP = 0;
      continue;
    }

    // Folded glyphs are drawn at smallCaps::SCALE, so all their metrics scale to match.
    const int glyphLeft = folded ? static_cast<int>(glyph.left * smallCaps::SCALE) : glyph.left;
    const int glyphWidth = folded ? static_cast<int>(glyph.width * smallCaps::SCALE + 0.5f) : glyph.width;
    const int glyphTop = folded ? static_cast<int>(glyph.top * smallCaps::SCALE) : glyph.top;
    const int glyphHeight = folded ? static_cast<int>(glyph.height * smallCaps::SCALE + 0.5f) : glyph.height;

    const int raiseBy = isCombining ? combiningMark::raiseAboveBase(glyphTop, glyphHeight, lastBaseTop) : 0;

    if (!isCombining && prevCp != 0) {
      auto kernFP = static_cast<int32_t>(getKerning(prevCp, cp));  // 4.4 fixed-point kern
      if (folded) kernFP = scaleAdvanceFP(kernFP);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
    }

    const int glyphBaseX =
        isCombining ? combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, glyphLeft, glyphWidth)
                    : lastBaseX;
    const int glyphBaseY = startY - raiseBy;

    *minX = std::min(*minX, glyphBaseX + glyphLeft);
    *maxX = std::max(*maxX, glyphBaseX + glyphLeft + glyphWidth);
    *minY = std::min(*minY, glyphBaseY + glyphTop - glyphHeight);
    *maxY = std::max(*maxY, glyphBaseY + glyphTop);

    if (!isCombining) {
      lastBaseLeft = glyphLeft;
      lastBaseWidth = glyphWidth;
      lastBaseAdvanceFP = folded ? scaleAdvanceFP(glyph.advanceX) : glyph.advanceX;  // 12.4 fixed-point
      lastBaseTop = glyphTop;
      prevAdvanceFP = lastBaseAdvanceFP;
      prevCp = cp;
    }
  }
}

void EpdFont::getTextDimensions(const char* string, int* w, int* h, const bool useSmallCaps) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  getTextBounds(string, 0, 0, &minX, &minY, &maxX, &maxY, useSmallCaps);

  *w = maxX - minX;
  *h = maxY - minY;
}

// Split form: the search touches only the codepoint array. See EpdFontData::kernLeftCodepoints.
static uint8_t lookupKernClassSplit(const uint16_t* codepoints, const uint8_t* classIds, const uint16_t count,
                                    const uint32_t cp) {
  if (!codepoints || count == 0 || cp > 0xFFFF) {
    return 0;
  }
  const auto target = static_cast<uint16_t>(cp);
  const uint16_t* end = codepoints + count;
  const auto it = std::lower_bound(codepoints, end, target);
  return (it != end && *it == target) ? classIds[it - codepoints] : 0;
}

static uint8_t lookupKernClass(const EpdKernClassEntry* entries, const uint16_t count, const uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) {
    return 0;
  }

  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;

  // lower_bound: exact-key lookup. Finds the first entry with codepoint >= target,
  // then the equality check confirms an exact match exists.
  const auto it = std::lower_bound(
      entries, end, target, [](const EpdKernClassEntry& entry, uint16_t value) { return entry.codepoint < value; });

  if (it != end && it->codepoint == target) {
    return it->classId;
  }

  return 0;
}

int8_t EpdFont::getKerning(const uint32_t leftCp, const uint32_t rightCp) const {
  // CJK text is set on a grid, so no pair touching a CJK character kerns. This also keeps CJK pairs
  // away from a TTF family's pair store, which they would fill with zeros. Ported from crosspoint-reader
  // PR #2288 (Justin Mitchell).
  if (utf8IsCjkBreakable(leftCp) || utf8IsCjkBreakable(rightCp)) return 0;
  const bool hasKernTables =
      (data->kernMatrix || data->kernRowOffsets) && (data->kernLeftClasses || data->kernLeftCodepoints);
  if (!hasKernTables) {
    return data->provider ? data->provider->kerning(leftCp, rightCp) : 0;
  }
  // Built-in fonts carry the split arrays, SD-card fonts the packed ones; never both.
  const bool split = data->kernLeftCodepoints != nullptr;
  const uint8_t lc =
      split ? lookupKernClassSplit(data->kernLeftCodepoints, data->kernLeftClassIds, data->kernLeftEntryCount, leftCp)
            : lookupKernClass(data->kernLeftClasses, data->kernLeftEntryCount, leftCp);
  if (lc == 0) return 0;
  const uint8_t rc = split ? lookupKernClassSplit(data->kernRightCodepoints, data->kernRightClassIds,
                                                  data->kernRightEntryCount, rightCp)
                           : lookupKernClass(data->kernRightClasses, data->kernRightEntryCount, rightCp);
  if (rc == 0) return 0;

  // Sparse (built-in fonts): scan the row. See EpdFontData::kernRowOffsets.
  //
  // Cost, host-measured against the real notosans_18_regular tables with the real function
  // (min of 9 runs), sparse matrix AND split class maps together against the original dense
  // form: +2.8-4.2% on the mix real text produces, +3.8-5.0% when every pair has both classes.
  // The sparse matrix alone was +6.2-6.9% / +11.0-12.9%; splitting the class maps paid most of
  // that back, because those two binary searches are ~96% of this function. Net: ~415 KB of
  // flash for a few percent on a ~30 ns call.
  if (data->kernRowOffsets) {
    const uint16_t begin = data->kernRowOffsets[lc - 1];
    const uint16_t end = data->kernRowOffsets[lc];
    const auto target = static_cast<uint8_t>(rc - 1);
    const uint8_t* cols = data->kernSparseCols;
    // Linear scan with an early exit, not a binary search: rows hold 14.5 entries on average
    // across the built-in set, short enough that the scan measured faster (worst-case mix +11%
    // over dense against +19% for std::lower_bound). It should widen on the C3, which reads
    // these arrays through the flash cache — the scan walks forwards through a cache line while
    // the search jumps around it.
    for (uint16_t i = begin; i < end; i++) {
      if (cols[i] == target) return data->kernSparseValues[i];
      if (cols[i] > target) break;  // sorted ascending, so past the target means absent
    }
    return 0;
  }

  // Dense (SD-card fonts, mapped straight out of the .cpfont).
  return data->kernMatrix[(lc - 1) * data->kernRightClassCount + (rc - 1)];
}

uint32_t EpdFont::getLigature(const uint32_t leftCp, const uint32_t rightCp) const {
  const auto* pairs = data->ligaturePairs;
  const auto count = data->ligaturePairCount;
  if (!pairs || count == 0 || leftCp > 0xFFFF || rightCp > 0xFFFF) {
    return 0;
  }

  const uint32_t key = (leftCp << 16) | rightCp;
  const auto* end = pairs + count;

  // lower_bound: exact-key lookup. Finds the first entry with pair >= key,
  // then the equality check confirms an exact match exists.
  const auto it =
      std::lower_bound(pairs, end, key, [](const EpdLigaturePair& pair, uint32_t value) { return pair.pair < value; });

  if (it != end && it->pair == key) {
    return it->ligatureCp;
  }

  return 0;
}

uint32_t EpdFont::applyLigatures(uint32_t cp, const char*& text) const {
  if (!data->ligaturePairs || data->ligaturePairCount == 0) {
    return cp;
  }
  while (true) {
    const auto saved = reinterpret_cast<const uint8_t*>(text);
    const uint32_t nextCp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text));
    if (nextCp == 0) break;
    const uint32_t lig = getLigature(cp, nextCp);
    if (lig == 0) {
      text = reinterpret_cast<const char*>(saved);
      break;
    }
    cp = lig;
  }
  return cp;
}

// The real lookup: interval table, then the on-demand loader an SD font
// installs. No fallbacks -- getGlyph() layers those on top, so a fallback can
// never recurse back into another fallback.
EpdGlyphRef EpdFont::findGlyph(const uint32_t cp) const {
  const int count = data->intervalCount;
  if (count == 0 && !data->provider) return {};

  if (count > 0) {
    const EpdUnicodeInterval* intervals = data->intervals;
    const auto* end = intervals + count;

    // upper_bound: range lookup. Finds the first interval with first > cp, so the
    // interval just before it is the last one with first <= cp. That's the only
    // candidate that could contain cp. Then we verify cp <= candidate.last.
    const auto it = std::upper_bound(
        intervals, end, cp, [](uint32_t value, const EpdUnicodeInterval& interval) { return value < interval.first; });

    if (it != intervals) {
      const auto& interval = *(it - 1);
      if (cp <= interval.last) {
        return epdResolveGlyph(data, interval.offset + (cp - interval.first));
      }
    }
  }

  // Codepoint not in the interval table: ask the font's provider (SD on-demand ring, or a font
  // with no tables at all).
  if (data->provider) {
    if (const EpdGlyph* loaded = data->provider->missGlyph(cp)) {
      // A provider's record, not an array one: there is no index to report, and the pointer is
      // what GlyphProvider::ownsBitmap() recognises it by.
      return EpdGlyphRef{loaded,
                         loaded->advanceX,
                         0,  // no array index: this glyph is not in the array
                         loaded->width,
                         loaded->height,
                         static_cast<int8_t>(loaded->left),
                         static_cast<int8_t>(loaded->top),
                         true};
    }
  }
  return {};
}

// Reached only when the interval table missed -- see the inline getGlyph() in the header.
EpdGlyphRef EpdFont::getGlyphSlow(const uint32_t cp) const {
  // The SD on-demand loader, which findGlyph() consults after the intervals.
  if (const EpdGlyphRef glyph = findGlyph(cp)) return glyph;

  // The font does not have it. Before giving up and drawing a box, try a close
  // relative that most fonts do carry — a dictionary's phonetic transcription is
  // otherwise a row of identical rectangles. See GlyphFallback.h; this only
  // runs once the real glyph has been ruled out, so a font that has the
  // character is never second-guessed.
  const uint32_t substitute = fallbackGlyphCodepoint(cp);
  if (substitute != cp) {
    if (const EpdGlyphRef glyph = findGlyph(substitute)) return glyph;
  }

  if (cp != REPLACEMENT_GLYPH) return findGlyph(REPLACEMENT_GLYPH);
  return {};
}

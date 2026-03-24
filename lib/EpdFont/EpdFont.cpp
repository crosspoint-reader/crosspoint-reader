#include "EpdFont.h"

#include <Utf8.h>

#include <algorithm>

void EpdFont::getTextBounds(const char* string, const int startX, const int startY, int* minX, int* minY, int* maxX,
                            int* maxY) const {
  *minX = startX;
  *minY = startY;
  *maxX = startX;
  *maxY = startY;

  if (*string == '\0') {
    return;
  }

  int32_t cursorXFP = fp4::fromPixel(startX);  // 12.4 fixed-point accumulator
  int lastBaseX = startX;
  int lastBaseAdvanceFP = 0;  // 12.4 fixed-point
  int lastBaseTop = 0;
  constexpr int MIN_COMBINING_GAP_PX = 1;
  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const bool isCombining = utf8IsCombiningMark(cp);

    if (!isCombining) {
      cp = applyLigatures(cp, string);
    }

    const EpdGlyph* glyph = getGlyph(cp);
    if (!glyph) {
      prevCp = 0;
      continue;
    }

    int raiseBy = 0;
    if (isCombining) {
      const int currentGap = glyph->top - glyph->height - lastBaseTop;
      if (currentGap < MIN_COMBINING_GAP_PX) {
        raiseBy = MIN_COMBINING_GAP_PX - currentGap;
      }
    }

    if (!isCombining && prevCp != 0) {
      cursorXFP += getKerning(prevCp, cp);  // 4.4 fixed-point kern
    }

    const int cursorXPixels = fp4::toPixel(cursorXFP);  // snap 12.4 fixed-point to nearest pixel
    const int glyphBaseX = isCombining ? (lastBaseX + fp4::toPixel(lastBaseAdvanceFP / 2)) : cursorXPixels;
    const int glyphBaseY = startY - raiseBy;

    *minX = std::min(*minX, glyphBaseX + glyph->left);
    *maxX = std::max(*maxX, glyphBaseX + glyph->left + glyph->width);
    *minY = std::min(*minY, glyphBaseY + glyph->top - glyph->height);
    *maxY = std::max(*maxY, glyphBaseY + glyph->top);

    if (!isCombining) {
      lastBaseX = cursorXPixels;
      lastBaseAdvanceFP = glyph->advanceX;  // 12.4 fixed-point
      lastBaseTop = glyph->top;
      cursorXFP += glyph->advanceX;  // 12.4 fixed-point advance
      prevCp = cp;
    }
  }
}

void EpdFont::getTextDimensions(const char* string, int* w, int* h) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  getTextBounds(string, 0, 0, &minX, &minY, &maxX, &maxY);

  *w = maxX - minX;
  *h = maxY - minY;
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
  if (!data->kernMatrix) {
    return 0;
  }
  const uint8_t lc = lookupKernClass(data->kernLeftClasses, data->kernLeftEntryCount, leftCp);
  if (lc == 0) return 0;
  const uint8_t rc = lookupKernClass(data->kernRightClasses, data->kernRightEntryCount, rightCp);
  if (rc == 0) return 0;
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

const EpdGlyph* EpdFont::getGlyph(const uint32_t cp) const {
  const int count = data->intervalCount;
  if (count == 0) return nullptr;

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
      return &data->glyph[interval.offset + (cp - interval.first)];
    }
  }

  if (cp != REPLACEMENT_GLYPH) {
    return getGlyph(REPLACEMENT_GLYPH);
  }
  return nullptr;
}

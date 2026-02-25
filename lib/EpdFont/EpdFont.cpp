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

  int cursorX = startX;
  const int cursorY = startY;
  int lastBaseX = startX;
  int lastBaseAdvance = 0;
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
      // TODO: Better handle this?
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
      cursorX += getKerning(prevCp, cp);
    }

    const int glyphBaseX = isCombining ? (lastBaseX + lastBaseAdvance / 2) : cursorX;
    const int glyphBaseY = cursorY - raiseBy;

    *minX = std::min(*minX, glyphBaseX + glyph->left);
    *maxX = std::max(*maxX, glyphBaseX + glyph->left + glyph->width);
    *minY = std::min(*minY, glyphBaseY + glyph->top - glyph->height);
    *maxY = std::max(*maxY, glyphBaseY + glyph->top);

    if (!isCombining) {
      lastBaseX = cursorX;
      lastBaseAdvance = glyph->advanceX;
      lastBaseTop = glyph->top;
      cursorX += glyph->advanceX;
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
  int left = 0;
  int right = static_cast<int>(count) - 1;
  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const uint16_t midCp = entries[mid].codepoint;
    if (midCp == target) {
      return entries[mid].classId;
    }
    if (midCp < target) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
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
  int left = 0;
  int right = static_cast<int>(count) - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const uint32_t midKey = pairs[mid].pair;
    if (midKey == key) {
      return pairs[mid].ligatureCp;
    }
    if (midKey < key) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
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
  const EpdUnicodeInterval* intervals = data->intervals;
  const int count = data->intervalCount;

  if (count == 0) return nullptr;

  // Binary search for O(log n) lookup instead of O(n)
  // Critical for Korean fonts with many unicode intervals
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];

    if (cp < interval->first) {
      right = mid - 1;
    } else if (cp > interval->last) {
      left = mid + 1;
    } else {
      // Found: cp >= interval->first && cp <= interval->last
      return &data->glyph[interval->offset + (cp - interval->first)];
    }
  }
  if (cp != REPLACEMENT_GLYPH) {
    return getGlyph(REPLACEMENT_GLYPH);
  }
  return nullptr;
}

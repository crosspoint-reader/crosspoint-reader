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
  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    cp = applyLigatures(cp, string);

    const EpdGlyph* glyph = getGlyph(cp);

    if (!glyph) {
      glyph = getGlyph(REPLACEMENT_GLYPH);
    }

    if (!glyph) {
      // TODO: Better handle this?
      prevCp = 0;
      continue;
    }

    if (prevCp != 0) {
      cursorX += getKerning(prevCp, cp);
    }

    *minX = std::min(*minX, cursorX + glyph->left);
    *maxX = std::max(*maxX, cursorX + glyph->left + glyph->width);
    *minY = std::min(*minY, cursorY + glyph->top - glyph->height);
    *maxY = std::max(*maxY, cursorY + glyph->top);
    cursorX += glyph->advanceX;
    prevCp = cp;
  }
}

void EpdFont::getTextDimensions(const char* string, int* w, int* h) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  getTextBounds(string, 0, 0, &minX, &minY, &maxX, &maxY);

  *w = maxX - minX;
  *h = maxY - minY;
}

template <typename T>
const T* binarySearchPairs(const T* pairs, const uint32_t pairCount, const uint32_t leftCp, const uint32_t rightCp) {
  if (!pairs || pairCount == 0) {
    return nullptr;
  }
  if (leftCp > 0xFFFF || rightCp > 0xFFFF) {
    return nullptr;
  }

  const uint32_t key = (leftCp << 16) | rightCp;
  int left = 0;
  int right = static_cast<int>(pairCount) - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const uint32_t midKey = pairs[mid].pair;
    if (midKey == key) {
      return &pairs[mid];
    }
    if (midKey < key) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }
  return nullptr;
}

int8_t EpdFont::getKerning(const uint32_t leftCp, const uint32_t rightCp) const {
  if (const auto* found = binarySearchPairs(data->kernPairs, data->kernPairCount, leftCp, rightCp)) {
    return found->adjust;
  }
  return 0;
}

uint32_t EpdFont::getLigature(const uint32_t leftCp, const uint32_t rightCp) const {
  if (const auto* found = binarySearchPairs(data->ligaturePairs, data->ligaturePairCount, leftCp, rightCp)) {
    return found->ligatureCp;
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

  return nullptr;
}

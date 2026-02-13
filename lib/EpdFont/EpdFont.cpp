#include "EpdFont.h"

#include <Utf8.h>

#include <algorithm>

void EpdFont::getTextBounds(const char* string, const int startX, const int startY, int* minX, int* minY, int* maxX,
                            int* maxY, const bool kerningEnabled) const {
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
    // Ligature chaining: substitute while pairs match
    while (kerningEnabled) {
      const auto saved = reinterpret_cast<const uint8_t*>(string);
      const uint32_t nextCp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string));
      if (nextCp == 0) break;
      const uint32_t lig = getLigature(cp, nextCp);
      if (lig == 0) {
        string = reinterpret_cast<const char*>(saved);
        break;
      }
      cp = lig;
    }

    const EpdGlyph* glyph = getGlyph(cp);

    if (!glyph) {
      glyph = getGlyph(REPLACEMENT_GLYPH);
    }

    if (!glyph) {
      // TODO: Better handle this?
      continue;
    }

    if (kerningEnabled && prevCp != 0) {
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

void EpdFont::getTextDimensions(const char* string, int* w, int* h, const bool kerningEnabled) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  getTextBounds(string, 0, 0, &minX, &minY, &maxX, &maxY, kerningEnabled);

  *w = maxX - minX;
  *h = maxY - minY;
}

bool EpdFont::hasPrintableChars(const char* string, const bool kerningEnabled) const {
  int w = 0, h = 0;

  getTextDimensions(string, &w, &h, kerningEnabled);

  return w > 0 || h > 0;
}

int8_t EpdFont::getKerning(const uint32_t leftCp, const uint32_t rightCp) const {
  if (!data->kernPairs || data->kernPairCount == 0) {
    return 0;
  }

  const uint32_t key = (leftCp << 16) | (rightCp & 0xFFFF);
  const EpdKernPair* pairs = data->kernPairs;
  int left = 0;
  int right = static_cast<int>(data->kernPairCount) - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    if (pairs[mid].pair == key) {
      return pairs[mid].adjust;
    }
    if (pairs[mid].pair < key) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }

  return 0;
}

uint32_t EpdFont::getLigature(const uint32_t leftCp, const uint32_t rightCp) const {
  if (!data->ligaturePairs || data->ligaturePairCount == 0) {
    return 0;
  }

  const uint32_t key = (leftCp << 16) | (rightCp & 0xFFFF);
  const EpdLigaturePair* pairs = data->ligaturePairs;
  int left = 0;
  int right = static_cast<int>(data->ligaturePairCount) - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    if (pairs[mid].pair == key) {
      return pairs[mid].ligatureCp;
    }
    if (pairs[mid].pair < key) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }

  return 0;
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

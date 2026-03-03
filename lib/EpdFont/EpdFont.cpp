#include "EpdFont.h"

#include <Logging.h>
#include <Utf8.h>

#include <algorithm>

namespace {
int computeAverageAdvanceX(const EpdFontData* data) {
  if (!data || !data->glyph || !data->intervals || data->intervalCount == 0) {
    return 8;
  }

  uint64_t advanceTotal = 0;
  uint32_t glyphCount = 0;
  for (uint32_t i = 0; i < data->intervalCount; i++) {
    const auto& interval = data->intervals[i];
    if (interval.last < interval.first) {
      continue;
    }

    const uint32_t span = interval.last - interval.first + 1;
    for (uint32_t j = 0; j < span; j++) {
      const uint16_t advanceX = data->glyph[interval.offset + j].advanceX;
      if (advanceX == 0) {
        continue;
      }
      advanceTotal += advanceX;
      glyphCount++;
    }
  }

  if (glyphCount == 0) {
    return 8;
  }
  return std::max(1, fp4::toPixel(static_cast<int32_t>(advanceTotal / glyphCount)));
}

uint8_t lookupKernClass(const EpdKernClassEntry* entries, const uint16_t count, const uint32_t cp) {
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
}  // namespace

void EpdFont::getTextBounds(const char* string, const int startX, const int startY, int* minX, int* minY, int* maxX,
                            int* maxY) const {
  *minX = startX;
  *minY = startY;
  *maxX = startX;
  *maxY = startY;

  if (*string == '\0') {
    return;
  }

  int32_t cursorXFP = fp4::fromPixel(startX);
  int averageAdvanceX = 0;
  bool averageAdvanceComputed = false;
  int lastBaseX = startX;
  int lastBaseAdvanceFP = 0;
  int lastBaseTop = 0;
  bool hasBaseGlyph = false;
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
      if (!averageAdvanceComputed) {
        averageAdvanceX = computeAverageAdvanceX(data);
        averageAdvanceComputed = true;
      }
      LOG_WRN("EPF", "Missing glyph U+%04lX; advancing by %dpx", static_cast<unsigned long>(cp), averageAdvanceX);
      if (!isCombining) {
        cursorXFP += fp4::fromPixel(averageAdvanceX);
        prevCp = 0;
      }
      continue;
    }

    int raiseBy = 0;
    if (isCombining && hasBaseGlyph) {
      const int currentGap = glyph->top - glyph->height - lastBaseTop;
      if (currentGap < MIN_COMBINING_GAP_PX) {
        raiseBy = MIN_COMBINING_GAP_PX - currentGap;
      }
    }

    if (!isCombining && prevCp != 0) {
      cursorXFP += getKerning(prevCp, cp);
    }

    const int cursorXPixels = fp4::toPixel(cursorXFP);
    const int glyphBaseX =
        (isCombining && hasBaseGlyph) ? (lastBaseX + fp4::toPixel(lastBaseAdvanceFP / 2)) : cursorXPixels;
    const int glyphBaseY = startY - raiseBy;

    *minX = std::min(*minX, glyphBaseX + glyph->left);
    *maxX = std::max(*maxX, glyphBaseX + glyph->left + glyph->width);
    *minY = std::min(*minY, glyphBaseY + glyph->top - glyph->height);
    *maxY = std::max(*maxY, glyphBaseY + glyph->top);

    if (!isCombining) {
      lastBaseX = cursorXPixels;
      lastBaseAdvanceFP = glyph->advanceX;
      lastBaseTop = glyph->top;
      hasBaseGlyph = true;
      cursorXFP += glyph->advanceX;
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

bool EpdFont::hasPrintableChars(const char* string) const {
  int w = 0, h = 0;
  getTextDimensions(string, &w, &h);
  return w > 0 || h > 0;
}

int EpdFont::getKerning(const uint32_t leftCp, const uint32_t rightCp) const {
  if (!data->kernMatrix) {
    return 0;
  }

  const uint8_t leftClass = lookupKernClass(data->kernLeftClasses, data->kernLeftEntryCount, leftCp);
  if (leftClass == 0) {
    return 0;
  }

  const uint8_t rightClass = lookupKernClass(data->kernRightClasses, data->kernRightEntryCount, rightCp);
  if (rightClass == 0) {
    return 0;
  }

  return data->kernMatrix[(leftClass - 1) * data->kernRightClassCount + (rightClass - 1)];
}

uint32_t EpdFont::getLigature(const uint32_t leftCp, const uint32_t rightCp) const {
  const auto* pairs = data->ligaturePairs;
  const auto pairCount = data->ligaturePairCount;
  if (!pairs || pairCount == 0 || leftCp > 0xFFFF || rightCp > 0xFFFF) {
    return 0;
  }

  const uint32_t key = (leftCp << 16) | rightCp;
  int left = 0;
  int right = static_cast<int>(pairCount) - 1;
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
    const auto* saved = reinterpret_cast<const uint8_t*>(text);
    const uint32_t nextCp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text));
    if (nextCp == 0) {
      break;
    }

    const uint32_t ligatureCp = getLigature(cp, nextCp);
    if (ligatureCp == 0) {
      text = reinterpret_cast<const char*>(saved);
      break;
    }

    cp = ligatureCp;
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

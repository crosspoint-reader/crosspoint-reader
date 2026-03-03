#include "SdFont.h"

#include <FeatureFlags.h>

#if ENABLE_USER_FONTS

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>

SdFont::SdFont() {
  fontData.bitmap = nullptr;
  fontData.glyph = nullptr;
  fontData.intervals = nullptr;
  fontData.intervalCount = 0;
  fontData.advanceY = 0;
  fontData.ascender = 0;
  fontData.descender = 0;
  fontData.is2Bit = false;
}

SdFont::~SdFont() { unload(); }

void SdFont::unload() {
  if (ownedBitmap) free(ownedBitmap);
  if (ownedGlyphs) free(ownedGlyphs);
  if (ownedIntervals) free(ownedIntervals);

  ownedBitmap = nullptr;
  ownedGlyphs = nullptr;
  ownedIntervals = nullptr;

  fontData.bitmap = nullptr;
  fontData.glyph = nullptr;
  fontData.intervals = nullptr;
  fontData.intervalCount = 0;

  loaded = false;
}

bool SdFont::load(const std::string& path) {
  unload();

  FsFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file) {
    LOG_ERR("SDFONT", "Failed to open font file: %s", path.c_str());
    return false;
  }

  // Header format:
  // 4 bytes: Magic "CPF\x01"
  // 1 byte: advanceY
  // 4 bytes (int32): ascender
  // 4 bytes (int32): descender
  // 1 byte: is2Bit (bool)
  // 4 bytes (uint32): intervalCount
  // 4 bytes (uint32): totalGlyphs
  // 4 bytes (uint32): bitmapSize

  auto readExact = [&file](void* dst, const size_t len) -> bool { return file.read(dst, len) == len; };

  char magic[4];
  if (!readExact(magic, sizeof(magic)) || memcmp(magic, "CPF\x01", 4) != 0) {
    LOG_ERR("SDFONT", "Invalid font magic in %s", path.c_str());
    return false;
  }

  uint8_t advanceY;
  int32_t ascender, descender;
  uint8_t is2Bit;
  uint32_t intervalCount, totalGlyphs, bitmapSize;

  if (!readExact(&advanceY, sizeof(advanceY)) || !readExact(&ascender, sizeof(ascender)) ||
      !readExact(&descender, sizeof(descender)) || !readExact(&is2Bit, sizeof(is2Bit)) ||
      !readExact(&intervalCount, sizeof(intervalCount)) || !readExact(&totalGlyphs, sizeof(totalGlyphs)) ||
      !readExact(&bitmapSize, sizeof(bitmapSize))) {
    LOG_ERR("SDFONT", "Truncated header in %s", path.c_str());
    return false;
  }

  constexpr uint32_t MAX_INTERVAL_COUNT = 4096;
  constexpr uint32_t MAX_GLYPH_COUNT = 65535;
  constexpr uint32_t MAX_BITMAP_SIZE = 8 * 1024 * 1024;
  if (intervalCount > MAX_INTERVAL_COUNT || totalGlyphs > MAX_GLYPH_COUNT || bitmapSize > MAX_BITMAP_SIZE) {
    LOG_ERR("SDFONT", "CPF header out of bounds in %s (intervals=%u glyphs=%u bitmap=%u)", path.c_str(), intervalCount,
            totalGlyphs, bitmapSize);
    return false;
  }

  const uint64_t expectedPayloadSize = static_cast<uint64_t>(intervalCount) * sizeof(EpdUnicodeInterval) +
                                       static_cast<uint64_t>(totalGlyphs) * sizeof(EpdGlyph) + bitmapSize;
  const uint64_t expectedFileSize = 22ULL + expectedPayloadSize;
  if (expectedFileSize != file.size()) {
    LOG_ERR("SDFONT", "Invalid CPF layout in %s (expected %llu bytes, got %u)", path.c_str(),
            static_cast<unsigned long long>(expectedFileSize), static_cast<unsigned>(file.size()));
    return false;
  }

  fontData.advanceY = advanceY;
  fontData.ascender = ascender;
  fontData.descender = descender;
  fontData.is2Bit = is2Bit != 0;
  fontData.intervalCount = intervalCount;

  // Allocate and read intervals
  ownedIntervals = (EpdUnicodeInterval*)malloc(intervalCount * sizeof(EpdUnicodeInterval));
  if (!ownedIntervals) goto oom;
  if (!readExact(ownedIntervals, intervalCount * sizeof(EpdUnicodeInterval))) {
    LOG_ERR("SDFONT", "Failed to read intervals in %s", path.c_str());
    unload();
    file.close();
    return false;
  }
  fontData.intervals = ownedIntervals;

  // Allocate and read glyphs
  ownedGlyphs = (EpdGlyph*)malloc(totalGlyphs * sizeof(EpdGlyph));
  if (!ownedGlyphs) goto oom;
  if (!readExact(ownedGlyphs, totalGlyphs * sizeof(EpdGlyph))) {
    LOG_ERR("SDFONT", "Failed to read glyph data in %s", path.c_str());
    unload();
    file.close();
    return false;
  }
  fontData.glyph = ownedGlyphs;

  // Allocate and read bitmaps
  ownedBitmap = (uint8_t*)malloc(bitmapSize);
  if (!ownedBitmap) goto oom;
  if (!readExact(ownedBitmap, bitmapSize)) {
    LOG_ERR("SDFONT", "Failed to read bitmap data in %s", path.c_str());
    unload();
    file.close();
    return false;
  }
  fontData.bitmap = ownedBitmap;

  file.close();
  loaded = true;
  LOG_INF("SDFONT", "Loaded font %s (%u bytes bitmap, %u glyphs)", path.c_str(), bitmapSize, totalGlyphs);
  return true;

oom:
  LOG_ERR("SDFONT", "Out of memory loading font %s", path.c_str());
  unload();
  file.close();
  return false;
}

void SdFont::getTextBounds(const char* string, const int startX, const int startY, int* minX, int* minY, int* maxX,
                           int* maxY) const {
  *minX = startX;
  *minY = startY;
  *maxX = startX;
  *maxY = startY;

  if (!loaded || *string == '\0') {
    return;
  }

  int32_t cursorXFP = fp4::fromPixel(startX);
  const int cursorY = startY;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const EpdGlyph* glyph = getGlyph(cp);

    if (!glyph) {
      glyph = getGlyph(REPLACEMENT_GLYPH);
    }

    if (!glyph) {
      continue;
    }

    const int cursorX = fp4::toPixel(cursorXFP);
    *minX = std::min(*minX, cursorX + glyph->left);
    *maxX = std::max(*maxX, cursorX + glyph->left + glyph->width);
    *minY = std::min(*minY, cursorY + glyph->top - glyph->height);
    *maxY = std::max(*maxY, cursorY + glyph->top);
    cursorXFP += glyph->advanceX;
  }
}

void SdFont::getTextDimensions(const char* string, int* w, int* h) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  getTextBounds(string, 0, 0, &minX, &minY, &maxX, &maxY);
  *w = maxX - minX;
  *h = maxY - minY;
}

bool SdFont::hasPrintableChars(const char* string) const {
  int w = 0, h = 0;
  getTextDimensions(string, &w, &h);
  return w > 0 || h > 0;
}

const EpdGlyph* SdFont::getGlyph(const uint32_t cp) const {
  if (!loaded) return nullptr;

  const EpdUnicodeInterval* intervals = fontData.intervals;
  const int count = fontData.intervalCount;

  if (count == 0) return nullptr;

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
      return &fontData.glyph[interval->offset + (cp - interval->first)];
    }
  }

  return nullptr;
}
#endif

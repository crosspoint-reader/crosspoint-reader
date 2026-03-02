#include "SdCardFont.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

// .cpfont magic bytes
static constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
static constexpr uint16_t CPFONT_VERSION_MIN = 1;
static constexpr uint16_t CPFONT_VERSION_MAX = 2;
static constexpr uint32_t HEADER_SIZE = 32;

SdCardFont::~SdCardFont() { freeAll(); }

void SdCardFont::freeAll() {
  freeMiniData();
  delete[] fullIntervals_;
  fullIntervals_ = nullptr;
  freeKernLigatureData();
  loaded_ = false;
}

void SdCardFont::freeKernLigatureData() {
  delete[] kernLeftClasses_;
  kernLeftClasses_ = nullptr;
  delete[] kernRightClasses_;
  kernRightClasses_ = nullptr;
  delete[] kernMatrix_;
  kernMatrix_ = nullptr;
  delete[] ligaturePairs_;
  ligaturePairs_ = nullptr;
  kernLigLoaded_ = false;
}

void SdCardFont::applyKernLigaturePointers(EpdFontData& data) const {
  data.kernLeftClasses = kernLeftClasses_;
  data.kernRightClasses = kernRightClasses_;
  data.kernMatrix = kernMatrix_;
  data.kernLeftEntryCount = header_.kernLeftEntryCount;
  data.kernRightEntryCount = header_.kernRightEntryCount;
  data.kernLeftClassCount = header_.kernLeftClassCount;
  data.kernRightClassCount = header_.kernRightClassCount;
  data.ligaturePairs = ligaturePairs_;
  data.ligaturePairCount = header_.ligaturePairCount;
}

bool SdCardFont::loadKernLigatureData() {
  if (kernLigLoaded_) return true;
  bool hasKern = header_.kernLeftEntryCount > 0;
  bool hasLig = header_.ligaturePairCount > 0;
  if (!hasKern && !hasLig) {
    kernLigLoaded_ = true;
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (hasKern) {
    kernLeftClasses_ = new (std::nothrow) EpdKernClassEntry[header_.kernLeftEntryCount];
    kernRightClasses_ = new (std::nothrow) EpdKernClassEntry[header_.kernRightEntryCount];
    uint32_t matrixSize = static_cast<uint32_t>(header_.kernLeftClassCount) * header_.kernRightClassCount;
    kernMatrix_ = new (std::nothrow) int8_t[matrixSize];

    if (!kernLeftClasses_ || !kernRightClasses_ || !kernMatrix_) {
      LOG_ERR("SDCF", "Failed to allocate kern data (%u+%u+%u bytes)",
              header_.kernLeftEntryCount * 3u, header_.kernRightEntryCount * 3u, matrixSize);
      freeKernLigatureData();
      file.close();
      return false;
    }

    file.seekSet(kernLeftFileOffset_);
    size_t leftSz = header_.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = header_.kernRightEntryCount * sizeof(EpdKernClassEntry);
    // Sections are contiguous: left classes, right classes, matrix
    if (file.read(reinterpret_cast<uint8_t*>(kernLeftClasses_), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(kernRightClasses_), rightSz) != static_cast<int>(rightSz) ||
        file.read(reinterpret_cast<uint8_t*>(kernMatrix_), matrixSize) != static_cast<int>(matrixSize)) {
      LOG_ERR("SDCF", "Failed to read kern data");
      freeKernLigatureData();
      file.close();
      return false;
    }
  }

  if (hasLig) {
    ligaturePairs_ = new (std::nothrow) EpdLigaturePair[header_.ligaturePairCount];
    if (!ligaturePairs_) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      freeKernLigatureData();
      file.close();
      return false;
    }
    file.seekSet(ligatureFileOffset_);
    size_t sz = header_.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(ligaturePairs_), sz) != static_cast<int>(sz)) {
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      freeKernLigatureData();
      file.close();
      return false;
    }
  }

  file.close();
  kernLigLoaded_ = true;

  // Update stubData_ so kern/ligature is available even before prewarm
  applyKernLigaturePointers(stubData_);

  LOG_DBG("SDCF", "Kern/lig loaded: kernL=%u, kernR=%u, ligs=%u",
          header_.kernLeftEntryCount, header_.kernRightEntryCount,
          header_.ligaturePairCount);
  return true;
}

void SdCardFont::freeMiniData() {
  delete[] miniIntervals_;
  miniIntervals_ = nullptr;
  delete[] miniGlyphs_;
  miniGlyphs_ = nullptr;
  delete[] miniBitmap_;
  miniBitmap_ = nullptr;
  miniIntervalCount_ = 0;
  miniGlyphCount_ = 0;
  memset(&miniData_, 0, sizeof(miniData_));
  epdFont_.data = &stubData_;
}

bool SdCardFont::load(const char* path) {
  freeAll();
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  FsFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    file.close();
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    file.close();
    return false;
  }

  uint16_t version = headerBuf[8] | (headerBuf[9] << 8);
  if (version < CPFONT_VERSION_MIN || version > CPFONT_VERSION_MAX) {
    LOG_ERR("SDCF", "Unsupported version: %u", version);
    file.close();
    return false;
  }

  uint16_t flags = headerBuf[10] | (headerBuf[11] << 8);
  header_.is2Bit = (flags & 1) != 0;
  header_.intervalCount = headerBuf[12] | (headerBuf[13] << 8) | (headerBuf[14] << 16) | (headerBuf[15] << 24);
  header_.glyphCount = headerBuf[16] | (headerBuf[17] << 8) | (headerBuf[18] << 16) | (headerBuf[19] << 24);
  header_.advanceY = headerBuf[20];
  header_.ascender = static_cast<int16_t>(headerBuf[21] | (headerBuf[22] << 8));
  header_.descender = static_cast<int16_t>(headerBuf[23] | (headerBuf[24] << 8));

  // v2 kern/ligature fields (bytes 25-31); for v1 these remain 0
  if (version >= 2) {
    header_.kernLeftEntryCount = headerBuf[25] | (headerBuf[26] << 8);
    header_.kernRightEntryCount = headerBuf[27] | (headerBuf[28] << 8);
    header_.kernLeftClassCount = headerBuf[29];
    header_.kernRightClassCount = headerBuf[30];
    header_.ligaturePairCount = headerBuf[31];
  } else {
    header_.kernLeftEntryCount = 0;
    header_.kernRightEntryCount = 0;
    header_.kernLeftClassCount = 0;
    header_.kernRightClassCount = 0;
    header_.ligaturePairCount = 0;
  }

  // Compute file offsets
  intervalsFileOffset_ = HEADER_SIZE;
  glyphsFileOffset_ = intervalsFileOffset_ + header_.intervalCount * sizeof(EpdUnicodeInterval);
  kernLeftFileOffset_ = glyphsFileOffset_ + header_.glyphCount * sizeof(EpdGlyph);
  kernRightFileOffset_ = kernLeftFileOffset_ + header_.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  kernMatrixFileOffset_ = kernRightFileOffset_ + header_.kernRightEntryCount * sizeof(EpdKernClassEntry);
  ligatureFileOffset_ = kernMatrixFileOffset_ +
                         static_cast<uint32_t>(header_.kernLeftClassCount) * header_.kernRightClassCount;
  bitmapFileOffset_ = ligatureFileOffset_ + header_.ligaturePairCount * sizeof(EpdLigaturePair);

  // Load full intervals into RAM
  fullIntervals_ = new (std::nothrow) EpdUnicodeInterval[header_.intervalCount];
  if (!fullIntervals_) {
    LOG_ERR("SDCF", "Failed to allocate %u intervals", header_.intervalCount);
    file.close();
    return false;
  }

  file.seekSet(intervalsFileOffset_);
  size_t intervalsBytes = header_.intervalCount * sizeof(EpdUnicodeInterval);
  if (file.read(reinterpret_cast<uint8_t*>(fullIntervals_), intervalsBytes) != static_cast<int>(intervalsBytes)) {
    LOG_ERR("SDCF", "Failed to read intervals");
    delete[] fullIntervals_;
    fullIntervals_ = nullptr;
    file.close();
    return false;
  }

  file.close();

  // Initialize stub data (empty font — all lookups return nullptr)
  // Kern/ligature data loaded lazily on first prewarm()
  memset(&stubData_, 0, sizeof(stubData_));
  stubData_.advanceY = header_.advanceY;
  stubData_.ascender = header_.ascender;
  stubData_.descender = header_.descender;
  stubData_.is2Bit = header_.is2Bit;

  epdFont_.data = &stubData_;
  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (%u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, "
          "kernL=%u, kernR=%u, ligs=%u)",
          path, header_.intervalCount, header_.glyphCount,
          header_.advanceY, header_.ascender, header_.descender,
          header_.kernLeftEntryCount, header_.kernRightEntryCount,
          header_.ligaturePairCount);
  return true;
}

int32_t SdCardFont::findGlobalGlyphIndex(uint32_t codepoint) const {
  // Binary search in full intervals
  int left = 0;
  int right = static_cast<int>(header_.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const auto& interval = fullIntervals_[mid];
    if (codepoint < interval.first) {
      right = mid - 1;
    } else if (codepoint > interval.last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval.offset + (codepoint - interval.first));
    }
  }
  return -1;
}

int SdCardFont::prewarm(const char* utf8Text, bool metadataOnly) {
  if (!loaded_) return -1;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text
  uint32_t codepoints[MAX_PAGE_GLYPHS];
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < MAX_PAGE_GLYPHS) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    // Check if already collected (linear scan is fine for ~200 codepoints)
    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Always include replacement character
  bool hasReplacement = false;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (codepoints[i] == REPLACEMENT_GLYPH) {
      hasReplacement = true;
      break;
    }
  }
  if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
    codepoints[cpCount++] = REPLACEMENT_GLYPH;
  }

  // Sort codepoints for ordered interval building
  std::sort(codepoints, codepoints + cpCount);

  // Step 2: Map codepoints to global glyph indices
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  CpGlyphMapping* mappings = new (std::nothrow) CpGlyphMapping[cpCount];
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array");
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  int missed = static_cast<int>(cpCount - validCount);

  if (validCount == 0) {
    delete[] mappings;
    epdFont_.data = &stubData_;
    stats_.prewarmTotalMs = millis() - startMs;
    return missed;
  }

  // Step 3: Build mini intervals from sorted codepoints
  // Merge consecutive codepoints into ranges
  freeMiniData();

  // Count intervals needed (worst case: each codepoint is its own interval)
  uint32_t intervalCapacity = validCount;
  miniIntervals_ = new (std::nothrow) EpdUnicodeInterval[intervalCapacity];
  if (!miniIntervals_) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals");
    delete[] mappings;
    return static_cast<int>(cpCount);
  }

  miniIntervalCount_ = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      miniIntervals_[miniIntervalCount_].first = mappings[rangeStart].codepoint;
      miniIntervals_[miniIntervalCount_].last = mappings[i - 1].codepoint;
      miniIntervals_[miniIntervalCount_].offset = rangeStart;
      miniIntervalCount_++;
      rangeStart = i;
    }
  }

  // Step 4: Allocate mini glyph array and bitmap
  miniGlyphCount_ = validCount;
  miniGlyphs_ = new (std::nothrow) EpdGlyph[miniGlyphCount_];
  if (!miniGlyphs_) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs");
    delete[] mappings;
    freeMiniData();
    return static_cast<int>(cpCount);
  }

  // Step 5: Read glyph metadata from SD card, sorted by global index for sequential I/O
  // Build sorted-by-globalIndex order for sequential reads
  uint32_t* readOrder = new (std::nothrow) uint32_t[validCount];
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order");
    delete[] mappings;
    freeMiniData();
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
  std::sort(readOrder, readOrder + validCount, [&](uint32_t a, uint32_t b) {
    return mappings[a].globalIndex < mappings[b].globalIndex;
  });

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm");
    delete[] readOrder;
    delete[] mappings;
    freeMiniData();
    return static_cast<int>(cpCount);
  }

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;

  // Read glyph metadata
  int32_t lastReadIndex = -1;
  for (uint32_t i = 0; i < validCount; i++) {
    uint32_t mapIdx = readOrder[i];
    int32_t gIdx = mappings[mapIdx].globalIndex;

    uint32_t fileOff = glyphsFileOffset_ + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
    // Only seek if not sequential
    if (gIdx != lastReadIndex + 1) {
      file.seekSet(fileOff);
      seekCount++;
    }
    file.read(reinterpret_cast<uint8_t*>(&miniGlyphs_[mapIdx]), sizeof(EpdGlyph));
    lastReadIndex = gIdx;
  }

  uint32_t totalBitmapSize = 0;

  if (!metadataOnly) {
    // Step 6: Compute total bitmap size needed
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += miniGlyphs_[i].dataLength;
    }

    miniBitmap_ = new (std::nothrow) uint8_t[totalBitmapSize > 0 ? totalBitmapSize : 1];
    if (!miniBitmap_) {
      LOG_ERR("SDCF", "Failed to allocate mini bitmap (%u bytes)", totalBitmapSize);
      file.close();
      delete[] readOrder;
      delete[] mappings;
      freeMiniData();
      return static_cast<int>(cpCount);
    }

    // Step 7: Read bitmap data sorted by file offset for sequential I/O
    // Sort by original dataOffset (file offset within bitmap section)
    std::sort(readOrder, readOrder + validCount, [&](uint32_t a, uint32_t b) {
      return miniGlyphs_[a].dataOffset < miniGlyphs_[b].dataOffset;
    });

    uint32_t miniBitmapOffset = 0;
    uint32_t lastBitmapEnd = UINT32_MAX;
    for (uint32_t i = 0; i < validCount; i++) {
      uint32_t mapIdx = readOrder[i];
      EpdGlyph& glyph = miniGlyphs_[mapIdx];

      if (glyph.dataLength == 0) {
        glyph.dataOffset = miniBitmapOffset;
        continue;
      }

      uint32_t fileOff = bitmapFileOffset_ + glyph.dataOffset;
      if (fileOff != lastBitmapEnd) {
        file.seekSet(fileOff);
        seekCount++;
      }
      file.read(miniBitmap_ + miniBitmapOffset, glyph.dataLength);
      lastBitmapEnd = fileOff + glyph.dataLength;

      // Update dataOffset to point into mini bitmap buffer
      glyph.dataOffset = miniBitmapOffset;
      miniBitmapOffset += glyph.dataLength;
    }
  }

  uint32_t sdTime = millis() - sdStart;
  file.close();
  delete[] readOrder;
  delete[] mappings;

  // Lazy-load kern/ligature data on first prewarm (not during load() to save RAM)
  loadKernLigatureData();

  // Step 8: Populate miniData_ and swap
  memset(&miniData_, 0, sizeof(miniData_));
  miniData_.bitmap = miniBitmap_;
  miniData_.glyph = miniGlyphs_;
  miniData_.intervals = miniIntervals_;
  miniData_.intervalCount = miniIntervalCount_;
  miniData_.advanceY = header_.advanceY;
  miniData_.ascender = header_.ascender;
  miniData_.descender = header_.descender;
  miniData_.is2Bit = header_.is2Bit;
  applyKernLigaturePointers(miniData_);

  epdFont_.data = &miniData_;

  // Update stats
  stats_.prewarmTotalMs = millis() - startMs;
  stats_.sdReadTimeMs = sdTime;
  stats_.seekCount = seekCount;
  stats_.uniqueGlyphs = validCount;
  stats_.bitmapBytes = totalBitmapSize;

  return missed;
}

void SdCardFont::clearCache() {
  freeMiniData();  // Also zeroes miniData_ and restores epdFont_.data = &stubData_
}

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes",
          label, stats_.prewarmTotalMs, stats_.sdReadTimeMs, stats_.seekCount,
          stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

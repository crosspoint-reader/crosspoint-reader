#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace IncrementalSection {

static constexpr uint32_t CACHE_MAGIC = 0x43504953U;  // CPIS
static constexpr uint8_t CACHE_VERSION = 1;
static constexpr size_t PAGE_INDEX_RECORD_SIZE = 16;

enum class CacheState : uint8_t {
  Building = 1,
  Complete = 2,
  Failed = 3,
};

struct LayoutCacheKey {
  uint8_t cacheVersion = CACHE_VERSION;
  int32_t fontId = 0;
  float lineCompression = 1.0F;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;

  bool matches(const LayoutCacheKey& other) const {
    return cacheVersion == other.cacheVersion && fontId == other.fontId &&
           lineCompression == other.lineCompression && extraParagraphSpacing == other.extraParagraphSpacing &&
           paragraphAlignment == other.paragraphAlignment && viewportWidth == other.viewportWidth &&
           viewportHeight == other.viewportHeight && hyphenationEnabled == other.hyphenationEnabled &&
           embeddedStyle == other.embeddedStyle && imageRendering == other.imageRendering &&
           focusReadingEnabled == other.focusReadingEnabled;
  }
};

struct Paths {
  std::string meta;
  std::string pages;
  std::string index;
  std::string anchors;
};

inline uint32_t knownPageCountFromIndexBytes(const size_t indexBytes) {
  if (indexBytes == 0 || indexBytes % PAGE_INDEX_RECORD_SIZE != 0) {
    return 0;
  }
  return static_cast<uint32_t>(indexBytes / PAGE_INDEX_RECORD_SIZE);
}

inline Paths pathsForSection(const std::string& sectionsDir, const uint32_t spineIndex) {
  const auto base = sectionsDir + "/" + std::to_string(spineIndex);
  return {base + ".met", base + ".pag", base + ".idx", base + ".anc"};
}

}  // namespace IncrementalSection

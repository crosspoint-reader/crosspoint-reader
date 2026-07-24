#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

int SdCardFontManager::loadFile(const SdCardFontFileInfo& file, const char* familyName, GfxRenderer& renderer) {
  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", file.path.c_str());
    return 0;
  }

  if (!font->load(file.path.c_str())) {
    LOG_ERR("SDMGR", "Failed to load %s", file.path.c_str());
    delete font;
    return 0;
  }

  int fontId = computeFontId(font->contentHash(), familyName, file.pointSize);
  // Guard against collision with built-in font IDs (astronomically unlikely
  // with FNV-1a hashes, but provides a safety net)
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, file.path.c_str());
    delete font;
    return 0;
  }
  renderer.registerSdCardFont(fontId, font);
  loaded_.push_back({font, fontId, file.pointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u", file.path.c_str(), file.pointSize, fontId, font->styleCount());

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily);
  return fontId;
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Select the physical point size closest to the built-in reader sizes. Some
  // CJK font packs only ship larger sizes, so ordinal selection can make
  // MEDIUM load 18pt+ and produce oversized pages on small devices.
  const SdCardFontFileInfo* selected = family.findClosestReaderSize(fontSizeEnum);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }

  if (loadFile(*selected, family.name.c_str(), renderer) == 0) {
    return false;
  }

  loadedFamilyName_ = family.name;
  loadedPointSize_ = selected->pointSize;
  return true;
}

int SdCardFontManager::loadFamilyExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                           uint8_t pointSize) {
  const SdCardFontFileInfo* file = family.findFile(pointSize);
  if (!file) return 0;  // family has no .cpfont at this exact size

  // Reuse an already-loaded font of the same size (e.g. when a reader size
  // happens to match a UI size) instead of double-loading the file.
  for (const auto& lf : loaded_) {
    if (lf.size == pointSize) return lf.fontId;
  }

  return loadFile(*file, family.name.c_str(), renderer);
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  // Drop UI CJK fallbacks before the SD fonts they point at are freed.
  renderer.clearFallbackFonts();
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  return loaded_.front().fontId;
}

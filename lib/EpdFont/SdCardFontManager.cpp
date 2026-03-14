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

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Load all font files from SD card
  loaded_.reserve(family.files.size());
  for (const auto& fileInfo : family.files) {
    auto* font = new (std::nothrow) SdCardFont();
    if (!font) {
      LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", fileInfo.path.c_str());
      continue;
    }

    if (!font->load(fileInfo.path.c_str())) {
      LOG_ERR("SDMGR", "Failed to load %s", fileInfo.path.c_str());
      delete font;
      continue;
    }

    // Check for duplicate point size (e.g. two files for the same family at the same size)
    bool duplicate = false;
    for (const auto& lf : loaded_) {
      if (lf.size == fileInfo.pointSize) {
        LOG_ERR("SDMGR", "Duplicate size %u for family %s, skipping %s", fileInfo.pointSize, family.name.c_str(),
                fileInfo.path.c_str());
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      delete font;
      continue;
    }

    int fontId = computeFontId(font->contentHash(), family.name.c_str(), fileInfo.pointSize);
    // Guard against collision with built-in font IDs (astronomically unlikely
    // with FNV-1a hashes, but provides a safety net)
    if (renderer.getFontMap().count(fontId) != 0) {
      LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, fileInfo.path.c_str());
      delete font;
      continue;
    }
    renderer.registerSdCardFont(fontId, font);
    loaded_.push_back({font, fontId, fileInfo.pointSize});

    LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u", fileInfo.path.c_str(), fileInfo.pointSize, fontId,
            font->styleCount());
  }

  if (loaded_.empty()) return false;

  // Build EpdFontFamily objects: each v4 SdCardFont has all styles
  for (const auto& lf : loaded_) {
    EpdFontFamily fontFamily(lf.font->getEpdFont(0), lf.font->getEpdFont(1), lf.font->getEpdFont(2),
                             lf.font->getEpdFont(3));
    renderer.insertFont(lf.fontId, fontFamily);
  }

  loadedFamilyName_ = family.name;
  return true;
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
}

int SdCardFontManager::getFontId(const std::string& familyName, uint8_t size, uint8_t /*style*/) const {
  if (familyName != loadedFamilyName_) return 0;
  for (const auto& lf : loaded_) {
    if (lf.size == size) return lf.fontId;
  }
  return 0;
}

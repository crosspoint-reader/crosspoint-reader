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

int SdCardFontManager::generateFontId(const std::string& name, uint8_t size, uint8_t style) {
  // DJB2 hash of "<name>_<size>_<style>" — deterministic and fast
  std::string key = name + "_" + std::to_string(size) + "_" + std::to_string(style);
  uint32_t hash = 5381;
  for (char c : key) {
    hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);
  }
  // Return as signed int (same range as built-in font IDs)
  return static_cast<int>(hash);
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Pass 1: Load all font files from SD card
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

    int fontId = generateFontId(family.name, fileInfo.pointSize, fileInfo.style);
    renderer.registerSdCardFont(fontId, font);
    loaded_.push_back({font, fontId, fileInfo.pointSize, fileInfo.style});

    LOG_DBG("SDMGR", "Loaded %s size=%u style=%u id=%d", fileInfo.path.c_str(), fileInfo.pointSize, fileInfo.style,
            fontId);
  }

  if (loaded_.empty()) return false;

  // Pass 2: Build EpdFontFamily objects for each regular-style font,
  // pairing with bold/italic/bold-italic variants if available at the same size.
  // The regular variant's fontId is used as the family key in the renderer's fontMap.
  for (const auto& lf : loaded_) {
    if (lf.style != 0) continue;  // Only create families from regular variants

    EpdFont* regular = lf.font->getEpdFont();
    EpdFont* bold = nullptr;
    EpdFont* italic = nullptr;
    EpdFont* boldItalic = nullptr;

    for (const auto& other : loaded_) {
      if (other.size != lf.size || other.style == 0) continue;
      if (other.style == 1) bold = other.font->getEpdFont();
      else if (other.style == 2) italic = other.font->getEpdFont();
      else if (other.style == 3) boldItalic = other.font->getEpdFont();
    }

    EpdFontFamily fontFamily(regular, bold, italic, boldItalic);
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

int SdCardFontManager::getFontId(const std::string& familyName, uint8_t size, uint8_t style) const {
  if (familyName != loadedFamilyName_) return 0;
  for (const auto& lf : loaded_) {
    if (lf.size == size && lf.style == style) return lf.fontId;
  }
  return 0;
}

float SdCardFontManager::getLineCompression(uint8_t lineSpacing) {
  // Same values as NotoSans
  switch (lineSpacing) {
    case 0:  // TIGHT
      return 0.90f;
    case 1:  // NORMAL
    default:
      return 0.95f;
    case 2:  // WIDE
      return 1.0f;
  }
}

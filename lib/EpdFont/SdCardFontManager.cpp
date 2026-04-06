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
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Single-variant loading: pick one base .cpfont file (prefer 14pt, else closest)
  static constexpr uint8_t PREFERRED_BASE_PT = 14;
  static constexpr uint8_t ALL_SIZES[] = {10, 12, 14, 16, 18};

  const SdCardFontFileInfo* bestFile = nullptr;
  int bestDiff = INT_MAX;
  for (const auto& fileInfo : family.files) {
    int diff = abs(static_cast<int>(fileInfo.pointSize) - PREFERRED_BASE_PT);
    if (diff < bestDiff) {
      bestDiff = diff;
      bestFile = &fileInfo;
    }
  }
  if (!bestFile) return false;

  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont");
    return false;
  }
  if (!font->load(bestFile->path.c_str())) {
    LOG_ERR("SDMGR", "Failed to load %s", bestFile->path.c_str());
    delete font;
    return false;
  }

  const uint8_t basePt = bestFile->pointSize;
  loaded_.push_back({font, 0, basePt});

  // Register the same SdCardFont under 4 virtual fontIds (one per target size)
  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1),
                           font->getEpdFont(2), font->getEpdFont(3));
  virtualFontIds_.clear();

  for (uint8_t targetPt : ALL_SIZES) {
    int fontId = computeFontId(font->contentHash(), family.name.c_str(), targetPt);
    if (renderer.getFontMap().count(fontId) != 0) {
      LOG_ERR("SDMGR", "Font ID %d collides, skipping size %u", fontId, targetPt);
      continue;
    }
    renderer.registerSdCardFont(fontId, font);
    renderer.insertFont(fontId, fontFamily);

    // Scale factor: 8.8 fixed-point (256 = 1.0x)
    uint16_t scale = static_cast<uint16_t>(static_cast<uint32_t>(targetPt) * 256 / basePt);
    renderer.registerSdCardFontScale(fontId, scale);
    virtualFontIds_.push_back(fontId);

    if (targetPt == basePt) {
      loaded_[0].fontId = fontId;
    }

    LOG_DBG("SDMGR", "Registered size=%u id=%d scale=%u/256 (base=%u)", targetPt, fontId, scale, basePt);
  }

  if (virtualFontIds_.empty()) {
    delete font;
    loaded_.clear();
    return false;
  }

  if (loaded_[0].fontId == 0 && !virtualFontIds_.empty()) {
    loaded_[0].fontId = virtualFontIds_[0];
  }

  loadedFamilyName_ = family.name;
  LOG_DBG("SDMGR", "Loaded %s (base=%upt, %zu virtual IDs)", family.name.c_str(), basePt, virtualFontIds_.size());
  return true;
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  // Remove all virtual fontIds from renderer maps
  for (int id : virtualFontIds_) {
    renderer.unregisterSdCardFont(id);
    renderer.removeFont(id);
  }
  renderer.clearSdCardFontScales();
  virtualFontIds_.clear();

  // Delete the single SdCardFont object
  for (auto& lf : loaded_) {
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
}

int SdCardFontManager::getFontId(const std::string& familyName, uint8_t size, uint8_t /*style*/) const {
  if (familyName != loadedFamilyName_) return 0;
  if (loaded_.empty()) return 0;
  // All sizes are registered as virtual fontIds using the same base font's contentHash
  return computeFontId(loaded_[0].font->contentHash(), familyName.c_str(), size);
}

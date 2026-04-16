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

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t preferredBasePt,
                                   uint8_t headingBasePt) {
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Dual-base loading: pick a primary .cpfont closest to preferredBasePt (body text),
  // and optionally a secondary .cpfont closest to headingBasePt (heading text).
  // Each virtual font ID is assigned to the closer base, eliminating upscaling artifacts.
  static constexpr uint8_t ALL_SIZES[] = {8, 10, 12, 14, 16, 18};

  // --- Primary base selection ---
  const SdCardFontFileInfo* primaryFile = nullptr;
  int bestDiff = INT_MAX;
  for (const auto& fileInfo : family.files) {
    int diff = abs(static_cast<int>(fileInfo.pointSize) - static_cast<int>(preferredBasePt));
    if (diff < bestDiff) {
      bestDiff = diff;
      primaryFile = &fileInfo;
    }
  }
  if (!primaryFile) return false;

  auto* primaryFont = new (std::nothrow) SdCardFont();
  if (!primaryFont) {
    LOG_ERR("SDMGR", "Failed to allocate primary SdCardFont");
    return false;
  }
  if (!primaryFont->load(primaryFile->path.c_str())) {
    LOG_ERR("SDMGR", "Failed to load %s", primaryFile->path.c_str());
    delete primaryFont;
    return false;
  }

  const uint8_t primaryPt = primaryFile->pointSize;
  loaded_.push_back({primaryFont, 0, primaryPt});

  // --- Secondary base selection (for headings) ---
  SdCardFont* secondaryFont = nullptr;
  uint8_t secondaryPt = 0;

  if (headingBasePt != 0 && headingBasePt != primaryPt) {
    const SdCardFontFileInfo* headingFile = nullptr;
    int headBestDiff = INT_MAX;
    for (const auto& fileInfo : family.files) {
      int diff = abs(static_cast<int>(fileInfo.pointSize) - static_cast<int>(headingBasePt));
      if (diff < headBestDiff) {
        headBestDiff = diff;
        headingFile = &fileInfo;
      }
    }

    // Only load secondary if it's a different file than primary
    if (headingFile && headingFile->path != primaryFile->path) {
      secondaryFont = new (std::nothrow) SdCardFont();
      if (secondaryFont) {
        if (secondaryFont->load(headingFile->path.c_str())) {
          secondaryPt = headingFile->pointSize;
          loaded_.push_back({secondaryFont, 0, secondaryPt});
          LOG_DBG("SDMGR", "Loaded secondary base: %s (%upt)", headingFile->path.c_str(), secondaryPt);
        } else {
          LOG_ERR("SDMGR", "Failed to load secondary %s, falling back to single base", headingFile->path.c_str());
          delete secondaryFont;
          secondaryFont = nullptr;
        }
      }
    }
  }

  // --- Register virtual font IDs ---
  virtualFontIds_.clear();

  for (uint8_t targetPt : ALL_SIZES) {
    // fontId is always computed from primary contentHash for getFontId() consistency
    int fontId = computeFontId(primaryFont->contentHash(), family.name.c_str(), targetPt);
    if (renderer.getFontMap().count(fontId) != 0) {
      LOG_ERR("SDMGR", "Font ID %d collides, skipping size %u", fontId, targetPt);
      continue;
    }

    // Choose base: use the closer one. On tie, prefer larger base (downscale > upscale).
    SdCardFont* chosenFont = primaryFont;
    uint8_t chosenPt = primaryPt;

    if (secondaryFont) {
      int diffPrimary = abs(static_cast<int>(targetPt) - static_cast<int>(primaryPt));
      int diffSecondary = abs(static_cast<int>(targetPt) - static_cast<int>(secondaryPt));
      if (diffSecondary < diffPrimary || (diffSecondary == diffPrimary && secondaryPt > primaryPt)) {
        chosenFont = secondaryFont;
        chosenPt = secondaryPt;
      }
    }

    EpdFontFamily fontFamily(chosenFont->getEpdFont(0), chosenFont->getEpdFont(1), chosenFont->getEpdFont(2),
                             chosenFont->getEpdFont(3));
    renderer.registerSdCardFont(fontId, chosenFont);
    renderer.insertFont(fontId, fontFamily);

    // Scale factor: 8.8 fixed-point (256 = 1.0x)
    uint16_t scale = static_cast<uint16_t>(static_cast<uint32_t>(targetPt) * 256 / chosenPt);
    renderer.registerSdCardFontScale(fontId, scale);
    virtualFontIds_.push_back(fontId);

    if (targetPt == primaryPt) {
      loaded_[0].fontId = fontId;
    }

    LOG_DBG("SDMGR", "Registered size=%u id=%d scale=%u/256 (base=%u%s)", targetPt, fontId, scale, chosenPt,
            chosenFont == secondaryFont ? " [heading]" : "");
  }

  if (virtualFontIds_.empty()) {
    for (auto& lf : loaded_) delete lf.font;
    loaded_.clear();
    return false;
  }

  if (loaded_[0].fontId == 0 && !virtualFontIds_.empty()) {
    loaded_[0].fontId = virtualFontIds_[0];
  }

  loadedFamilyName_ = family.name;
  loadedBasePt_ = primaryPt;
  loadedHeadingBasePt_ = secondaryPt;
  LOG_DBG("SDMGR", "Loaded %s (primary=%upt, heading=%upt, %zu virtual IDs)", family.name.c_str(), primaryPt,
          secondaryPt, virtualFontIds_.size());
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
  loadedBasePt_ = 0;
  loadedHeadingBasePt_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName, uint8_t size, uint8_t /*style*/) const {
  if (familyName != loadedFamilyName_) return 0;
  if (loaded_.empty()) return 0;
  // All sizes are registered as virtual fontIds using the same base font's contentHash
  return computeFontId(loaded_[0].font->contentHash(), familyName.c_str(), size);
}

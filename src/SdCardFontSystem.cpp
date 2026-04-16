#include "SdCardFontSystem.h"

#include <FontManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"

// Map fontSize enum (SMALL=0, MEDIUM=1, LARGE=2, EXTRA_LARGE=3) to point sizes.
// Index 4 (TABLE_SIZE) maps to 10pt for table rendering.
// Index 5 (RUBY_SIZE) maps to 8pt for ruby/furigana text.
static constexpr uint8_t FONT_SIZE_TO_PT[] = {12, 14, 16, 18, 10, 8};

// Convert fontSize enum to the point size used as .cpfont base selection target.
static uint8_t fontSizeEnumToPt(uint8_t fontSizeEnum) {
  if (fontSizeEnum < sizeof(FONT_SIZE_TO_PT)) return FONT_SIZE_TO_PT[fontSizeEnum];
  return 14;  // fallback
}

// Compute the heading base point size for the largest heading (h1, sizeStep=2).
// Returns 0 if heading size equals body size (no secondary base needed).
static uint8_t computeHeadingBasePt(uint8_t fontSizeEnum) {
  uint8_t headingEnum = std::min<uint8_t>(fontSizeEnum + 2, CrossPointSettings::EXTRA_LARGE);
  if (headingEnum == fontSizeEnum) return 0;
  return fontSizeEnumToPt(headingEnum);
}

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.horizontal.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.horizontal.sdFontFamilyName);
    if (family) {
      uint8_t basePt = fontSizeEnumToPt(SETTINGS.horizontal.fontSize);
      uint8_t headingPt = computeHeadingBasePt(SETTINGS.horizontal.fontSize);
      if (manager_.loadFamily(*family, renderer, basePt, headingPt)) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s (base=%upt, heading=%upt)", SETTINGS.horizontal.sdFontFamilyName,
                basePt, headingPt);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.horizontal.sdFontFamilyName);
        SETTINGS.horizontal.sdFontFamilyName[0] = '\0';
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.horizontal.sdFontFamilyName);
      SETTINGS.horizontal.sdFontFamilyName[0] = '\0';
    }
  }

  // Suppress ExternalFont reader rendering when SD card font is active
  FontManager::getInstance().setSdCardFontActive(SETTINGS.horizontal.sdFontFamilyName[0] != '\0');

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, bool isVertical) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  if (registryDirty_.exchange(false, std::memory_order_acquire)) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  auto& ds = SETTINGS.getDirectionSettings(isVertical);
  const char* wantedFamily = ds.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    FontManager::getInstance().setSdCardFontActive(false);
    return;
  }

  const uint8_t wantedBasePt = fontSizeEnumToPt(ds.fontSize);
  const uint8_t wantedHeadingPt = computeHeadingBasePt(ds.fontSize);
  const bool familyChanged = (currentFamily != wantedFamily);
  const bool basePtChanged = (manager_.loadedBasePt() != wantedBasePt);
  const bool headingPtChanged = (manager_.loadedHeadingBasePt() != wantedHeadingPt);

  if (!familyChanged && !basePtChanged && !headingPtChanged) return;

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, wantedBasePt, wantedHeadingPt)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s (base=%upt, heading=%upt)", wantedFamily, wantedBasePt,
              wantedHeadingPt);
      FontManager::getInstance().setSdCardFontActive(true);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      ds.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    ds.sdFontFamilyName[0] = '\0';
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t fontSizeEnum) const {
  if (fontSizeEnum >= sizeof(FONT_SIZE_TO_PT)) return 0;
  uint8_t ptSize = FONT_SIZE_TO_PT[fontSizeEnum];

  int fontId = manager_.getFontId(familyName, ptSize, 0);
  if (fontId != 0) return fontId;

  // Requested size not available — find closest available size
  const auto* family = registry_.findFamily(familyName);
  if (!family) return 0;

  auto sizes = family->availableSizes();
  if (sizes.empty()) return 0;

  uint8_t bestSize = sizes[0];
  int bestDiff = abs(static_cast<int>(ptSize) - static_cast<int>(bestSize));
  for (uint8_t s : sizes) {
    int diff = abs(static_cast<int>(ptSize) - static_cast<int>(s));
    if (diff < bestDiff) {
      bestDiff = diff;
      bestSize = s;
    }
  }
  return manager_.getFontId(familyName, bestSize, 0);
}

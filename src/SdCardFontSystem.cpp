#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"

// Map fontSize enum (SMALL=0, MEDIUM=1, LARGE=2, EXTRA_LARGE=3) to point sizes.
static constexpr uint8_t FONT_SIZE_TO_PT[] = {12, 14, 16, 18};

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer)) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  if (registryDirty_.exchange(false, std::memory_order_acquire)) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  if (currentFamily == wantedFamily) return;

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
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

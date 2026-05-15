#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <climits>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "SdCardFontGlobals.h"

// Free-function resolver used by CrossPointSettings::getReaderFontId().
// Resolved by the linker — no callback indirection stored in settings.
int resolveSdCardFontId(const char* familyName, uint8_t fontSizeEnum) {
  return sdFontSystem.resolveFontId(familyName, fontSizeEnum);
}

// --- Font-family dynamic SettingInfo trampolines ---
//
// The font-family SettingInfo lives in a namespace-static SettingsList that is
// initialized at global-static phase, well before sdFontSystem.begin() runs.
// We therefore cannot bake the SD family list into the SettingInfo at
// construction; instead the SettingInfo holds these stateless trampolines that
// consult sdFontSystem at every call. enumLabels is enriched lazily by the
// consumers (SettingsActivity, CrossPointWebServer) before each iteration.

uint8_t fontFamilyDynamicGetter(const void* /*ctx*/) {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto& families = sdFontSystem.registry().getFamilies();
    for (size_t i = 0; i < families.size(); i++) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
        return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
      }
    }
    // SD family no longer present (card removed?); fall through to built-in.
  }
  return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily
                                                                      : CrossPointSettings::BOOKERLY;
}

void fontFamilyDynamicSetter(void* /*ctx*/, uint8_t value) {
  if (value < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.fontFamily = value;
    SETTINGS.sdFontFamilyName[0] = '\0';
    return;
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = value - CrossPointSettings::BUILTIN_FONT_COUNT;
  if (sdIdx < families.size()) {
    strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  }
}

uint8_t txtFontFamilyDynamicGetter(const void* /*ctx*/) {
  if (SETTINGS.txtSdFontFamilyName[0] != '\0') {
    const auto& families = sdFontSystem.registry().getFamilies();
    for (size_t i = 0; i < families.size(); i++) {
      if (families[i].name == SETTINGS.txtSdFontFamilyName) {
        return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
      }
    }
  }
  return SETTINGS.txtFontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.txtFontFamily
                                                                         : CrossPointSettings::NOTOSANS;
}

void txtFontFamilyDynamicSetter(void* /*ctx*/, uint8_t value) {
  if (value < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.txtFontFamily = value;
    SETTINGS.txtSdFontFamilyName[0] = '\0';
    return;
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = value - CrossPointSettings::BUILTIN_FONT_COUNT;
  if (sdIdx < families.size()) {
    strncpy(SETTINGS.txtSdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.txtSdFontFamilyName) - 1);
    SETTINGS.txtSdFontFamilyName[sizeof(SETTINGS.txtSdFontFamilyName) - 1] = '\0';
  }
}

uint8_t fontFamilyOptionCount() {
  return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + sdFontSystem.registry().getFamilies().size());
}

std::string fontFamilyOptionLabel(uint8_t i) {
  if (i < CrossPointSettings::BUILTIN_FONT_COUNT) {
    static const StrId BUILTIN_LABELS[] = {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS};
    return I18N.get(BUILTIN_LABELS[i]);
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = i - CrossPointSettings::BUILTIN_FONT_COUNT;
  return sdIdx < families.size() ? families[sdIdx].name : std::string();
}

// Map fontSize enum (SMALL=0, MEDIUM=1, LARGE=2, EXTRA_LARGE=3, TINY=4) to point sizes.
static constexpr uint8_t FONT_SIZE_TO_PT[] = {12, 14, 16, 18, 10};

static uint8_t targetPtSizeFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= sizeof(FONT_SIZE_TO_PT)) e = 1;  // default to MEDIUM
  return FONT_SIZE_TO_PT[e];
}

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, targetPtSizeFromSettings())) {
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
  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t targetPt = targetPtSizeFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size changed and the
  // family has a closer file than what's currently loaded.
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
    const uint8_t bestPt = best ? best->pointSize : 0;
    if (bestPt == manager_.currentPointSize()) return;  // already loaded with the right size
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (target %u)", wantedFamily, manager_.currentPointSize(), bestPt,
            targetPt);
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, targetPt)) {
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

static uint8_t targetPtSizeFromEnum(uint8_t fontSizeEnum) {
  if (fontSizeEnum >= sizeof(FONT_SIZE_TO_PT)) fontSizeEnum = 1;  // default to MEDIUM
  return FONT_SIZE_TO_PT[fontSizeEnum];
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, const char* wantedFamily, uint8_t fontSizeEnum) {
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t targetPt = targetPtSizeFromEnum(fontSizeEnum);

  if (!wantedFamily || wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) manager_.unloadAll(renderer);
    return;
  }

  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      manager_.unloadAll(renderer);
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
    if (best && best->pointSize == manager_.currentPointSize()) return;
  }

  if (!currentFamily.empty()) manager_.unloadAll(renderer);

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (!manager_.loadFamily(*family, renderer, targetPt)) {
      LOG_ERR("SDFS", "Failed to load SD font family: %s", wantedFamily);
    }
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t fontSizeEnum) const {
  // The manager loads exactly one size for the active SD family. Resolve only
  // if the requested family matches the loaded family and the requested size
  // matches the loaded size. otherwise return 0 so callers can fall back.
  if (!familyName || familyName[0] == '\0') return 0;
  if (manager_.currentFamilyName() != familyName) return 0;
  if (manager_.currentPointSize() != targetPtSizeFromEnum(fontSizeEnum)) return 0;
  return manager_.getFontId(familyName);
}

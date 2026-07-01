#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"

namespace {

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();
  dropCapRegistry_.discoverDropCaps();

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
      if (manager_.loadFamily(*family, renderer, fontSizeEnumFromSettings())) {
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

  ensureDropCapLoaded(renderer);

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureDropCapLoaded(GfxRenderer& renderer) {
  // The drop-cap face is the user-selected font from the standalone /.dropcap
  // registry (SETTINGS.dropCapFontName), independent of the reader font. When
  // drop caps are off or no font is selected, load nothing (id 0) and the reader
  // integer-scales the body glyph instead.
  const char* name = (SETTINGS.dropCapsEnabled && SETTINGS.dropCapFontName[0] != '\0') ? SETTINGS.dropCapFontName : "";
  loadDropCapFamily(renderer, name);
}

int SdCardFontSystem::loadDropCapFamily(GfxRenderer& renderer, const char* name) {
  const SdCardFontFamilyInfo* family = (name && name[0] != '\0') ? dropCapRegistry_.findFamily(name) : nullptr;
  if (!family || family->files.empty()) {
    if (!dropCapManager_.currentFamilyName().empty()) {
      dropCapManager_.unloadAll(renderer);
    }
    renderer.setDropCapFontId(0);
    return 0;
  }

  // Drop-cap faces are large (e.g. 60-76pt), so map the reader's size enum
  // ordinally onto the family's available sizes rather than by nearest reading
  // size (which would always collapse to the smallest face).
  const uint8_t sizeEnum = fontSizeEnumFromSettings();
  const auto sizes = family->availableSizes();  // ascending
  uint8_t idx = sizeEnum;
  if (idx >= sizes.size()) idx = sizes.empty() ? 0 : static_cast<uint8_t>(sizes.size() - 1);
  const uint8_t wantedPt = sizes.empty() ? 0 : sizes[idx];

  // Already loaded for this family at the right size — just (re)publish the id.
  if (dropCapManager_.currentFamilyName() == family->name && wantedPt == dropCapManager_.currentPointSize()) {
    const int id = dropCapManager_.getFontId(family->name);
    renderer.setDropCapFontId(id);
    return id;
  }

  // Constrain a synthetic family to the single ordinal-selected file so
  // loadFamily() (which picks the nearest reading size) loads exactly that face.
  SdCardFontFamilyInfo one;
  one.name = family->name;
  if (const auto* f = family->findFile(wantedPt)) one.files.push_back(*f);

  if (!dropCapManager_.currentFamilyName().empty()) {
    dropCapManager_.unloadAll(renderer);
  }
  if (!one.files.empty() && dropCapManager_.loadFamily(one, renderer, sizeEnum)) {
    const int id = dropCapManager_.getFontId(one.name);
    renderer.setDropCapFontId(id);
    LOG_DBG("SDFS", "Loaded drop-cap face %s (pt %u)", one.name.c_str(), dropCapManager_.currentPointSize());
    return id;
  }
  renderer.setDropCapFontId(0);
  LOG_ERR("SDFS", "Failed to load drop-cap face %s", name);
  return 0;
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
    dropCapRegistry_.discoverDropCaps();
  }

  // Keep the drop-cap face in step regardless of which body font is selected.
  // Done before the body-font early-returns below so it always runs.
  ensureDropCapLoaded(renderer);

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
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

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

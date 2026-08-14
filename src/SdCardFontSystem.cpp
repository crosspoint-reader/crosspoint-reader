#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Each is paired with a same-size SD fallback so CJK UI
// text matches the surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();
  dropCapRegistry_.discoverDropCaps();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
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
  // size (which would always collapse to the smallest face). Develop tracks the
  // reader size as a point size, so derive the ordinal from its position among
  // the reader's available point sizes.
  const auto readerSizes = readerFontPointSizes(&registry_, SETTINGS.sdFontFamilyName);
  const uint8_t curReaderPt = snapToNearestPointSize(readerSizes, SETTINGS.fontPointSize);
  uint8_t sizeEnum = 0;
  for (size_t i = 0; i < readerSizes.size(); ++i) {
    if (readerSizes[i] == curReaderPt) {
      sizeEnum = static_cast<uint8_t>(i);
      break;
    }
  }
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

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
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
      SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.clearSdFontFamily();
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on CJK codepoints, so a Latin-only family
  // can never act as a fallback and its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script: Han, Hiragana, Katakana, Hangul.
  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}

#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <I18n.h>
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

// SD font family used to back a CJK UI language when the active reading
// font doesn't already provide CJK coverage. Must match the `name:` field
// in sd-fonts.yaml. See ensureUiLanguageFallback().
constexpr const char* kDefaultUiCjkFamily = "NotoSansCJKtc";

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

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

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
  // begin() runs from setupDisplayAndFonts(), called after main.cpp's setup()
  // has already loaded SETTINGS and called I18N.setLanguage() -- so the
  // saved language (including a CJK one) is already active here.
  if (!ensureUiLanguageFallback(renderer)) {
    // The SD card/font was removed since zh-Hant was saved (LanguageSelectActivity
    // guards this at selection time, but nothing guards the file disappearing
    // afterward). Revert before the first UI render rather than booting into a
    // language with no usable glyphs for its own menus.
    LOG_ERR("SDFS", "zh-Hant CJK fallback unavailable at boot - reverting UI language to English");
    I18N.setLanguage(Language::EN);
    SETTINGS.language = static_cast<uint8_t>(Language::EN);
    SETTINGS.saveToFile();
  }
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  ensureReadingFont(renderer);
  // A reading-font change can flip whether it covers CJK, or release/claim
  // the same family+size the UI-language fallback uses, so re-check on every
  // call regardless of which ensureReadingFont() branch fired.
  ensureUiLanguageFallback(renderer);
}

void SdCardFontSystem::ensureReadingFont(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

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
    // SD font ids are deterministic (content hash + family + size, see
    // SdCardFontManager::computeFontId), so loading the same family+size the
    // UI-language fallback already holds would collide with its own
    // registration and fail. Release that copy first — ensureUiLanguageFallback()
    // (called right after this function) reloads it if the load below
    // doesn't end up covering CJK after all.
    if (uiLangFallbackFamily_ == wantedFamily) {
      uiLangFallbackManager_.unloadAll(renderer);
      uiLangFallbackFamily_.clear();
    }
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

bool SdCardFontSystem::readingFontHasCjkCoverage(const GfxRenderer& renderer) const {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return false;

  // resolveTextFontId only redirects on CJK codepoints, so a Latin-only
  // family can never act as a fallback and its UI sizes would be dead
  // weight in RAM — probe before paying for them.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return false;
  // One representative codepoint per script: Han, Hiragana, Katakana, Hangul.
  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) return true;
  }
  return false;
}

bool SdCardFontSystem::readingFontCoversZhHant(const GfxRenderer& renderer) const {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return false;

  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return false;
  // Stricter than readingFontHasCjkCoverage(): that check accepts any single
  // CJK script and is fine for its purpose (deciding whether to fall back a
  // book title/filename to the reading font — a wrong guess there only
  // affects one string). A Kana- or Hangul-only reading font would pass that
  // check yet have no Han glyphs at all, which would leave every zh-Hant
  // menu string unrendered if it were accepted here.
  return readerIt->second.hasCodepoint(0x4E2D);  // 中 - present in any Han-literate font
}

bool SdCardFontSystem::loadUiFallbackSizes(SdCardFontManager& mgr, const SdCardFontFamilyInfo& family,
                                           GfxRenderer& renderer) {
  bool allLoaded = true;
  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = mgr.loadFamilyExtraSize(family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, family.name.c_str());
      allLoaded = false;
    }
  }
  return allLoaded;
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  if (!readingFontHasCjkCoverage(renderer)) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  loadUiFallbackSizes(manager_, *family, renderer);
}

bool SdCardFontSystem::ensureUiLanguageFallback(GfxRenderer& renderer) {
  // Only zh-Hant needs this today; add to this list when another CJK
  // translation ships (see gen_cjk_ui_intervals.py for the matching
  // built-in-font-side change).
  const bool needsCjk = I18N.getLanguage() == Language::ZH_HANT;

  // The active reading font already covers zh-Hant specifically (e.g. the
  // user also picked NotoSansCJKtc as their reading font) - setupUiFallbacks()
  // already wires it up. Drop our own independent copy if we're holding one
  // (e.g. the user just switched to such a reading font): loading it again
  // here would waste SD I/O and RAM for the exact same glyphs. Deliberately
  // stricter than setupUiFallbacks()'s own readingFontHasCjkCoverage() check
  // (see readingFontCoversZhHant()) since a wrong "yes" here drops the
  // entire menu's coverage, not just one book title.
  const bool readingFontCovers = needsCjk && readingFontCoversZhHant(renderer);

  if (!needsCjk || readingFontCovers) {
    if (!uiLangFallbackFamily_.empty()) {
      uiLangFallbackManager_.unloadAll(renderer);
      uiLangFallbackFamily_.clear();
      // unloadAll() above is scoped to uiLangFallbackManager_'s own fonts
      // (see SdCardFontManager::unloadAll), so the reading font's own
      // fallback registration, if any, is untouched by it.
    }
    return true;  // not needed, or already covered by the reading font
  }

  const auto* family = registry_.findFamily(kDefaultUiCjkFamily);
  if (!family) {
    LOG_DBG("SDFS", "%s not installed - zh-Hant menus will show missing glyphs until it is", kDefaultUiCjkFamily);
    return false;
  }

  // Re-assert unconditionally (not just when uiLangFallbackFamily_ was still
  // empty): this keeps the fallback registration idempotent rather than
  // trusting a cached flag to reflect what's actually registered.
  // loadFamilyExtraSize() reuses an already-loaded size, so repeat calls are
  // cheap.
  if (!loadUiFallbackSizes(uiLangFallbackManager_, *family, renderer)) {
    // A partial install (e.g. a manually side-loaded .cpfont missing a size)
    // must not be accepted as "covered" -- most UI text would still show
    // missing glyphs. Reject the whole family rather than leaving some sizes
    // registered and others not.
    LOG_ERR("SDFS", "%s is missing required UI sizes - rejecting incomplete zh-Hant fallback", kDefaultUiCjkFamily);
    uiLangFallbackManager_.unloadAll(renderer);
    uiLangFallbackFamily_.clear();
    return false;
  }
  uiLangFallbackFamily_ = kDefaultUiCjkFamily;
  return true;
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}

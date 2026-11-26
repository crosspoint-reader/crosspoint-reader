#include "SdCardFontSystem.h"

#include <EpdFontFamily.h>
#include <Fnv1a.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <IndicScripts.h>
#include <Logging.h>
#include <Memory.h>
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "BookFontSystem.h"
#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

// Out-of-line ctor/dtor: TtfEpdFont is complete here, so unique_ptr<TtfEpdFont>
// can be constructed/destroyed. (Declared in the header where it is only
// forward-declared.)
SdCardFontSystem::SdCardFontSystem() = default;
SdCardFontSystem::~SdCardFontSystem() = default;

namespace {

#if CROSSPOINT_VECTOR_FONTS
int computeTtfFontId(const char* familyName, const uint32_t contentKey, const uint8_t pointSize,
                     const bool uiFallback = false) {
  static constexpr char VERSION_SALT[] = "TTF-content-v1";
  const char* name = familyName ? familyName : "";
  uint32_t hash = fnv1a::hash(VERSION_SALT, sizeof(VERSION_SALT));
  hash = fnv1a::hash(name, strlen(name) + 1, hash);
  const uint8_t fields[] = {static_cast<uint8_t>(contentKey >> 24),
                            static_cast<uint8_t>(contentKey >> 16),
                            static_cast<uint8_t>(contentKey >> 8),
                            static_cast<uint8_t>(contentKey),
                            pointSize,
                            static_cast<uint8_t>(uiFallback)};
  hash = fnv1a::hash(fields, sizeof(fields), hash);
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

// Read big-endian uint16 / uint32 from an sfnt header buffer.
uint16_t readBe16(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }
uint32_t readBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

constexpr uint32_t sfntTag(const char a, const char b, const char c, const char d) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(d));
}

// Compute the prefix byte count needed to cover the font's lookup/metric tables
// (cmap, loca, hmtx, kern, etc.) without caching the multi-MB glyf/CFF body.
// Falls back to min(fileLen, maxPrefix) on TTC collections or unexpected layouts.
size_t computeTtfPrefixBytes(HalFile& file, const size_t fileLen, const size_t maxPrefix) {
  const size_t fallback = std::min(fileLen, maxPrefix);
  uint8_t hdr[12];
  if (!file.seek(0) || file.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return fallback;

  const uint32_t version = readBe32(hdr);
  if (version != 0x00010000u && version != sfntTag('O', 'T', 'T', 'O') && version != sfntTag('t', 'r', 'u', 'e') &&
      version != sfntTag('t', 'y', 'p', '1')) {
    return fallback;  // TTC ('ttcf') or unrecognized container
  }
  const uint16_t numTables = readBe16(hdr + 4);
  if (numTables == 0 || numTables > 128) return fallback;

  const size_t dirEnd = sizeof(hdr) + static_cast<size_t>(numTables) * 16u;
  if (dirEnd > fileLen) return fallback;

  size_t neededEnd = dirEnd;
  bool foundCoreTable = false;
  uint8_t records[16 * 16];  // 256 B stack chunk (16 table records)
  uint16_t remaining = numTables;
  while (remaining > 0) {
    const uint16_t batch = remaining < 16 ? remaining : 16;
    const size_t bytes = static_cast<size_t>(batch) * 16u;
    if (file.read(records, bytes) != static_cast<int>(bytes)) return fallback;
    for (uint16_t i = 0; i < batch; ++i) {
      const uint8_t* rec = records + static_cast<size_t>(i) * 16u;
      const uint32_t tag = readBe32(rec);
      const uint32_t offset = readBe32(rec + 8);
      const uint32_t length = readBe32(rec + 12);
      if (offset > fileLen || length > fileLen - offset) continue;
      const size_t end = static_cast<size_t>(offset) + length;
      switch (tag) {
        case sfntTag('h', 'e', 'a', 'd'):
        case sfntTag('h', 'h', 'e', 'a'):
        case sfntTag('m', 'a', 'x', 'p'):
        case sfntTag('O', 'S', '/', '2'):
        case sfntTag('c', 'm', 'a', 'p'):
        case sfntTag('l', 'o', 'c', 'a'):
        case sfntTag('h', 'm', 't', 'x'):
        case sfntTag('k', 'e', 'r', 'n'):
        case sfntTag('f', 'v', 'a', 'r'):
        case sfntTag('a', 'v', 'a', 'r'):
        case sfntTag('c', 'v', 't', ' '):
        case sfntTag('f', 'p', 'g', 'm'):
        case sfntTag('p', 'r', 'e', 'p'):
          if (end > maxPrefix) {
            if (offset < maxPrefix) return fallback;
            break;
          }
          neededEnd = std::max(neededEnd, end);
          foundCoreTable = true;
          break;
        default:
          break;
      }
    }
    remaining -= batch;
  }
  return foundCoreTable ? neededEnd : fallback;
}
#endif  // CROSSPOINT_VECTOR_FONTS

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
// SD-font converter). Each is paired with a same-size SD fallback so UI text
// in scripts the built-ins lack (CJK, Greek, Cyrillic, ...) matches the
// surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
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

void SdCardFontSystem::begin(GfxRenderer& renderer, const EpdFontFamily& small, const EpdFontFamily& ui10,
                             const EpdFontFamily& ui12) {
  builtinUiFonts_[0] = &small;
  builtinUiFonts_[1] = &ui10;
  builtinUiFonts_[2] = &ui12;
  refreshRegistry();
  renderer_ = &renderer;
  cpfontVariants_.reserve(MAX_CPFONT_VARIANTS);
#if CROSSPOINT_VECTOR_FONTS
  ttfVariants_.reserve(MAX_TTF_VARIANTS);
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->setReleaseCallback(
        [](void* ctx) {
          auto* self = static_cast<SdCardFontSystem*>(ctx);
          if (self->renderer_) self->clearTtfVariants(*self->renderer_);
        },
        this);
  }
#endif
  FontVariantProvider provider;
  provider.ctx = this;
  provider.resolve = [](void* ctx, const int fontId, const float scale) {
    return static_cast<SdCardFontSystem*>(ctx)->resolveVariant(fontId, scale);
  };
  provider.load = [](void* ctx, const int fontId) {
#if CROSSPOINT_VECTOR_FONTS
    if (bookFontSystem.load(fontId)) return true;
#endif
    return static_cast<SdCardFontSystem*>(ctx)->loadVariant(fontId);
  };
#if CROSSPOINT_VECTOR_FONTS
  provider.resolveFamily = [](void*, const uint32_t family, const float scale) {
    return bookFontSystem.resolve(family, scale);
  };
#endif
  renderer.setFontVariantProvider(provider);

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
#if CROSSPOINT_VECTOR_FONTS
      if (family->vector) {
        // Vector (.ttf/.otf) families load through the FreeInkFont path; the
        // .cpfont manager below rejects them ("Invalid magic bytes") and would
        // wipe the user's selection on every boot.
        loadTtfFamily(*family, renderer, /*registryWasDirty=*/false, /*setupFallbacks=*/false);
      } else
#endif
          if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      handleMissingFamily();
    }
  }

  // The optional UI faces must fit alongside the saved reader font.
  applyUiFont(renderer, SETTINGS.uiFontFamilyName);
  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::refreshIfDirty() {
  if (registryDirty_.exchange(false, std::memory_order_acquire)) refreshRegistry();
}

void SdCardFontSystem::refreshRegistry() {
#if CROSSPOINT_VECTOR_FONTS
  scanSavedFamilies(/*automaticRetry=*/false);
#else
  registry_.discover();
#endif
}

#if CROSSPOINT_VECTOR_FONTS
void SdCardFontSystem::scanSavedFamilies(const bool automaticRetry) {
  // The same scan serves both roles that were already waiting for discovery.
  readerFontRetrySpent_ = automaticRetry && (readerFontRetrySpent_ || readerFontUnavailable_);
  uiFontRetrySpent_ = automaticRetry && (uiFontRetrySpent_ || uiFontRecoveryPending_);

  SdCardFontRegistry::SavedFamilies saved{SETTINGS.sdFontFamilyName, SETTINGS.uiFontFamilyName,
                                          /*retain=*/automaticRetry};
  registry_.discover(&saved);

  readerFontUnavailable_ = saved.readerUnavailable;
  if (saved.uiUnavailable) {
    uiFontRecoveryPending_ = true;
  } else if (saved.ui.empty() || !registry_.findFamily(saved.ui)) {
    uiFontRecoveryPending_ = false;
  }
}
#endif

void SdCardFontSystem::handleMissingFamily() const {
#if CROSSPOINT_VECTOR_FONTS
  if (readerFontUnavailable_) {
    LOG_DBG("SDFS", "SD font scan incomplete; keeping selection: %s", SETTINGS.sdFontFamilyName);
    return;
  }
#endif
  LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
  SETTINGS.clearSdFontFamily();
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, [[maybe_unused]] const bool retryUnavailable) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Re-discovering fonts");
    refreshRegistry();
  }

#if CROSSPOINT_VECTOR_FONTS
  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const char* savedUiFamily = SETTINGS.uiFontFamilyName;
  if (!*savedUiFamily || (uiFontRecoveryPending_ && strcmp(savedUiFamily, activeUiFontName_) == 0)) {
    uiFontRecoveryPending_ = false;
  }
  if (readerFontUnavailable_ && (!*wantedFamily || registry_.findFamily(wantedFamily))) {
    readerFontUnavailable_ = false;
  }
  const bool retryReader = retryUnavailable && !registryWasDirty && readerFontUnavailable_ && !readerFontRetrySpent_;
  if (retryReader) {
    scanSavedFamilies(/*automaticRetry=*/true);
  }
#endif

  ensureReaderFontLoaded(renderer, registryWasDirty);

#if CROSSPOINT_VECTOR_FONTS
  if (retryUnavailable && uiFontRecoveryPending_) {
    if (!registryWasDirty && !retryReader && !uiFontRetrySpent_ && !registry_.findFamily(savedUiFamily)) {
      // A UI-only retry must not force a working reader font to reload.
      scanSavedFamilies(/*automaticRetry=*/true);
    }
    if (registry_.findFamily(savedUiFamily)) {
      // Attempt once after the reader loads, so it has first claim on memory.
      uiFontRecoveryPending_ = false;
      applyUiFont(renderer, savedUiFamily);
    }
  }
#endif
}

void SdCardFontSystem::ensureReaderFontLoaded(GfxRenderer& renderer, const bool registryWasDirty) {
  const char* wantedFamily = SETTINGS.sdFontFamilyName;
#if CROSSPOINT_VECTOR_FONTS
  // Vector (.ttf/.otf) family selected: route through the FreeInkFont path and
  // drop any pre-rasterized (.cpfont) font that was loaded.
  if (wantedFamily[0] != '\0') {
    const auto* wantedFam = registry_.findFamily(wantedFamily);
    if (wantedFam && wantedFam->vector) {
      if (!manager_.currentFamilyName().empty()) unloadCpfonts(renderer);
      loadTtfFamily(*wantedFam, renderer, registryWasDirty);
      return;
    }
    // An opportunistic scan can lose a family whose open source files still work.
    if (!wantedFam && readerFontUnavailable_ && !registryWasDirty && ttf_ && ttfFamily_ == wantedFamily &&
        ttfPointSize_ == SETTINGS.fontPointSize) {
      return;
    }
  }
  // Not on a vector family — ensure any previously-loaded TTF font is released
  // before the pre-rasterized/built-in path below takes over.
  if (!ttfFamily_.empty()) unloadTtf(renderer);
#endif

  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      unloadCpfonts(renderer);
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
      unloadCpfonts(renderer);
      handleMissingFamily();
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
    unloadCpfonts(renderer);
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
    handleMissingFamily();
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty() || activeUiFontName_[0] != '\0') return;

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on codepoints the built-in UI fonts lack,
  // so a family with no coverage beyond theirs can never act as a fallback and
  // its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script the built-in fonts may lack,
  // matching the non-Latin interval presets the SD-font converter ships
  // (docs/sd-card-fonts.md).
  static constexpr uint32_t kFallbackProbes[] = {
      0x4E00,  // CJK Unified Ideographs (Han)
      0x3042,  // Hiragana
      0x30A2,  // Katakana
      0xAC00,  // Hangul Syllables
      0x03B1,  // Greek
      0x0430,  // Cyrillic
      0x05D0,  // Hebrew
      0x0627,  // Arabic
      0x0531,  // Armenian
      0x10D0,  // Georgian
      0x1200,  // Ethiopic
      0x13A0,  // Cherokee
      0x2D30,  // Tifinagh
      0x0259,  // IPA Extensions (ə)
  };
  bool hasFallbackScript = false;
  for (const uint32_t cp : kFallbackProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasFallbackScript = true;
      break;
    }
  }
  // Every Indic script too.
  hasFallbackScript = hasFallbackScript ||
                      std::any_of(std::begin(indic::SCRIPTS), std::end(indic::SCRIPTS),
                                  [&](const indic::ScriptInfo& s) { return readerIt->second.hasCodepoint(s.probe); });
  if (!hasFallbackScript) {
    LOG_DBG("SDFS", "%s has no fallback-script coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    const auto& ui = kUiFontSizes[i];
    // Use the persisted CSS variant id so UI and reader text share one prewarm set.
    const int id = variantFontId(manager_.getFontId(familyName), ui.pointSize);
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize, id);
    if (sdFontId != 0) {
      cpfontUiIds_[i] = sdFontId;
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::variantFontId(const int readerFontId, const uint8_t pointSize) {
  uint32_t hash = 2166136261u;
  for (int shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<uint8_t>(static_cast<uint32_t>(readerFontId) >> shift);
    hash *= 16777619u;
  }
  hash ^= pointSize;
  hash *= 16777619u;
  hash ^= 0x56415200u;  // "VAR\0" salt keeps variants apart from reader and UI ids
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

void SdCardFontSystem::releaseForStorage(GfxRenderer& renderer) {
#if CROSSPOINT_VECTOR_FONTS
  unloadTtf(renderer);
#endif
  unloadCpfonts(renderer);
  applyUiFont(renderer, "");
}

void SdCardFontSystem::unloadCpfonts(GfxRenderer& renderer) {
  renderer.clearFallbackFonts();
  std::fill(std::begin(cpfontUiIds_), std::end(cpfontUiIds_), 0);
  cpfontVariants_.clear();  // the manager owns and frees the fonts themselves
  manager_.unloadAll(renderer);
}

void SdCardFontSystem::clearCpfontUiFallbacks(GfxRenderer& renderer) {
  const int readerId = manager_.getFontId(manager_.currentFamilyName());
  for (int& id : cpfontUiIds_) {
    if (id != 0 && id != readerId) {
      std::erase_if(cpfontVariants_, [id](const CpfontVariant& variant) { return variant.fontId == id; });
      manager_.unloadFont(renderer, id);
    }
    id = 0;
  }
}

const char* SdCardFontSystem::activeUiFontName() const {
#if CROSSPOINT_VECTOR_FONTS
  if (uiTtfFailed()) return "";
#endif
  return activeUiFontName_;
}

bool SdCardFontSystem::supportsUiFont(const SdCardFontFamilyInfo& family) {
  if (family.vector) return CROSSPOINT_VECTOR_FONTS;
  return std::all_of(std::begin(kUiFontSizes), std::end(kUiFontSizes),
                     [&](const UiFontSize& ui) { return family.hasSize(ui.pointSize); });
}

bool SdCardFontSystem::loadUiCpfonts(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  static constexpr size_t kMinInternalFree = 50 * 1024;
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    const auto& ui = kUiFontSizes[i];
    renderer.removeFont(ui.fontId);
    if (!uiCpfonts_.loadFamilyVariant(family, renderer, ui.pointSize, ui.fontId, builtinUiFonts_[i])) return false;
    if (HalMemory::getInternalHeap().freeBytes < kMinInternalFree) {
      LOG_ERR("SDFS", "Not enough internal heap for the UI bitmap font");
      return false;
    }
  }
  return true;
}

bool SdCardFontSystem::applyUiFont(GfxRenderer& renderer, const char* want) {
#if CROSSPOINT_VECTOR_FONTS
  unloadUiTtf(renderer);
  uiTtfFailed_.store(false, std::memory_order_relaxed);
#endif
  uiCpfonts_.unloadAll(renderer);
  activeUiFontName_[0] = '\0';
  if (*want) {
    const auto* family = registry_.findFamily(want);
    if (family && supportsUiFont(*family)) {
      // An explicit UI family uses the built-ins for missing glyphs. Release
      // reader-based fallbacks first to make room for the new font.
      renderer.clearFallbackFonts();
      clearCpfontUiFallbacks(renderer);
#if CROSSPOINT_VECTOR_FONTS
      clearTtfUiFallbacks(renderer);
      const bool loaded = family->vector ? loadUiTtf(*family, renderer) : loadUiCpfonts(*family, renderer);
#else
      const bool loaded = loadUiCpfonts(*family, renderer);
#endif
      if (loaded) {
#if CROSSPOINT_VECTOR_FONTS
        uiFontRecoveryPending_ = false;
#endif
        snprintf(activeUiFontName_, sizeof(activeUiFontName_), "%s", want);
        LOG_DBG("SDFS", "UI font: %s", want);
        return true;
      }
    }
    LOG_ERR("SDFS", "UI font '%s' missing or failed to load; using built-in", want);
#if CROSSPOINT_VECTOR_FONTS
    unloadUiTtf(renderer);
    uiTtfFailed_.store(false, std::memory_order_relaxed);
#endif
    uiCpfonts_.unloadAll(renderer);
  }
  restoreBuiltinUiFonts(renderer);
  setupUiFallbacks(renderer);
#if CROSSPOINT_VECTOR_FONTS
  setupTtfUiFallbacks(renderer);
#endif
  return *want == '\0';
}

void SdCardFontSystem::restoreBuiltinUiFonts(GfxRenderer& renderer) {
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    renderer.removeFont(kUiFontSizes[i].fontId);
    renderer.insertFont(kUiFontSizes[i].fontId, *builtinUiFonts_[i]);
  }
}

int SdCardFontSystem::resolveVariant(const int fontId, const float scale) {
  if (const int builtin = builtinReaderFontVariant(fontId, scale); builtin != 0) {
    return renderer_->getFontMap().count(builtin) ? builtin : 0;
  }
#if CROSSPOINT_VECTOR_FONTS
  if (ttf_ && fontId == ttfFontId_) {
    // Vector faces render at any size.
    const int pointSize = std::clamp(static_cast<int>(ttfPointSize_ * scale + 0.5f), 6, 72);
    if (pointSize == ttfPointSize_) return fontId;
    const int id = variantFontId(fontId, static_cast<uint8_t>(pointSize));
    return loadTtfVariant(static_cast<uint8_t>(pointSize), id) ? id : 0;
  }
#endif
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty() || fontId != manager_.getFontId(familyName)) return 0;
  const auto* family = registry_.findFamily(familyName);
  if (!family) return 0;

  // Pick among the sizes installed for the family: shrinking takes the largest size at or
  // below the target so small print never rounds back up to body size; growing takes the
  // nearest.
  const uint8_t base = manager_.currentPointSize();
  const float target = base * scale;
  uint8_t chosen = base;
  if (scale < 1.0f) {
    uint8_t atOrBelow = 0;
    uint8_t smallest = base;
    for (const auto& file : family->files) {
      if (file.pointSize <= target && file.pointSize > atOrBelow) atOrBelow = file.pointSize;
      smallest = std::min(smallest, file.pointSize);
    }
    chosen = atOrBelow != 0 ? atOrBelow : smallest;
  } else {
    for (const auto& file : family->files) {
      if (std::fabs(file.pointSize - target) < std::fabs(chosen - target)) chosen = file.pointSize;
    }
  }
  if (chosen == base) return fontId;
  const int id = variantFontId(fontId, chosen);
  return loadCpfontVariant(*family, chosen, id) ? id : 0;
}

bool SdCardFontSystem::loadVariant(const int fontId) {
#if CROSSPOINT_VECTOR_FONTS
  if (ttf_) {
    for (int pointSize = 6; pointSize <= 72; ++pointSize) {
      if (variantFontId(ttfFontId_, static_cast<uint8_t>(pointSize)) == fontId) {
        return loadTtfVariant(static_cast<uint8_t>(pointSize), fontId);
      }
    }
    return false;
  }
#endif
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return false;
  const auto* family = registry_.findFamily(familyName);
  if (!family) return false;
  const int readerFontId = manager_.getFontId(familyName);
  for (const auto& file : family->files) {
    if (variantFontId(readerFontId, file.pointSize) == fontId) {
      return loadCpfontVariant(*family, file.pointSize, fontId);
    }
  }
  return false;
}

bool SdCardFontSystem::loadCpfontVariant(const SdCardFontFamilyInfo& family, const uint8_t pointSize,
                                         const int fontId) {
  for (auto& variant : cpfontVariants_) {
    if (variant.fontId == fontId) {
      variant.lastUse = ++variantClock_;
      return true;
    }
  }
  // UI fallback sizes stay resident and must not enter the variant eviction list.
  if (manager_.getFontId(family.name, pointSize) == fontId) return true;
  // Each size keeps its interval tables resident (small for Latin, tens of KB for broad
  // CJK subsets), so skip it on a tight heap: the text then stays at the reader size.
  static constexpr size_t MIN_VARIANT_FREE_BLOCK = 48 * 1024;
  if (ESP.getMaxAllocHeap() < MIN_VARIANT_FREE_BLOCK) {
    LOG_DBG("SDFS", "Skipping %upt variant: largest free block %u", pointSize,
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  if (cpfontVariants_.size() >= MAX_CPFONT_VARIANTS) {
    const auto oldest = std::min_element(cpfontVariants_.begin(), cpfontVariants_.end(),
                                         [](const auto& a, const auto& b) { return a.lastUse < b.lastUse; });
    manager_.unloadFont(*renderer_, oldest->fontId);
    cpfontVariants_.erase(oldest);
  }
  if (!manager_.loadFamilyVariant(family, *renderer_, pointSize, fontId)) return false;
  cpfontVariants_.push_back({fontId, ++variantClock_});
  LOG_DBG("SDFS", "Loaded %s %upt variant (heap free %u)", family.name.c_str(), pointSize,
          static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
#if CROSSPOINT_VECTOR_FONTS
  // A loaded vector (.ttf) family answers first — it isn't in the .cpfont manager.
  if (ttfFontId_ != 0 && familyName && ttfFamily_ == familyName) return ttfFontId_;
#endif
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}

#if CROSSPOINT_VECTOR_FONTS

SdCardFontSystem::TtfSource* SdCardFontSystem::unusedTtfSources(const TtfSource* other) {
  return other == sourceSlots_[0] ? sourceSlots_[1] : sourceSlots_[0];
}

bool SdCardFontSystem::canShareTtfSources(const TtfSource* sources) const {
  if (!sources) return false;
  const uint32_t generation = sourceSlotGenerations_[sources == sourceSlots_[1]];
  return generation != 0 && generation == registry_.scanGeneration();
}

void SdCardFontSystem::releaseTtfSources(TtfSource*& sources, const TtfSource* other) {
  if (!sources || sources == other) {
    sources = nullptr;
    return;
  }
  sourceSlotGenerations_[sources == sourceSlots_[1]] = 0;
  for (uint8_t st = 0; st < 4; ++st) {
    TtfSource& s = sources[st];
    s.bytes.clear();
    freeink::font::PsramVector<uint8_t>().swap(s.bytes);  // actually release
    if (s.file) s.file.close();
    s.streamed = false;
    s.size = 0;
    s.present = false;
  }
  sources = nullptr;
}

bool SdCardFontSystem::loadTtfVariant(const uint8_t pointSize, const int fontId) {
  if (!ttf_) return false;
  for (auto& variant : ttfVariants_) {
    if (variant.fontId == fontId) {
      variant.lastUse = ++variantClock_;
      return true;
    }
  }
  if (ttfVariants_.size() >= MAX_TTF_VARIANTS) {
    const auto oldest = std::min_element(ttfVariants_.begin(), ttfVariants_.end(),
                                         [](const auto& a, const auto& b) { return a.lastUse < b.lastUse; });
    renderer_->unregisterTtfFont(oldest->fontId);
    renderer_->removeFont(oldest->fontId);
    ttfVariants_.erase(oldest);
  }
  auto font = makeUniqueNoThrow<TtfEpdFont>();
  if (!font) {
    LOG_ERR("SDFS", "OOM: TtfEpdFont variant @%upt", pointSize);
    return false;
  }
  // Variants carry headings and inline runs, sharing ttf_'s FreeType faces.
  if (!font->loadVariantOf(*ttf_, pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/64 * 1024, /*maxGlyphs=*/512))
    return false;
  font->build(" ");
  renderer_->insertFont(fontId, font->family());
  renderer_->registerTtfFont(fontId, font.get());
  ttfVariants_.push_back({fontId, ++variantClock_, std::move(font)});
  return true;
}

void SdCardFontSystem::clearTtfVariants(GfxRenderer& renderer) {
  for (const auto& variant : ttfVariants_) {
    renderer.unregisterTtfFont(variant.fontId);
    renderer.removeFont(variant.fontId);
  }
  ttfVariants_.clear();
}

void SdCardFontSystem::unloadTtf(GfxRenderer& renderer) {
  clearTtfVariants(renderer);  // they borrow ttfSources_
  if (ttfFamily_.empty() && ttfFontId_ == 0 && ttfUiIds_.empty()) return;
  clearTtfUiFallbacks(renderer);
  if (ttfFontId_ != 0) {
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);  // drop from the renderer's fontMap
  }
  ttf_.reset();  // frees the FT faces first (they read ttfSources_)
  releaseTtfSources(ttfSources_, uiTtfSources_);
  ttfFamily_.clear();
  ttfFontId_ = 0;
  ttfPointSize_ = 0;
}

bool SdCardFontSystem::openTtfSource(TtfSource& s, const std::string& path) {
  HalFile f = Storage.open(path.c_str());
  if (!f) {
    LOG_ERR("SDFS", "Failed to open TTF: %s", path.c_str());
    return false;
  }
  const size_t len = f.size();
  if (len == 0) {
    LOG_ERR("SDFS", "Empty TTF: %s", path.c_str());
    f.close();
    return false;
  }
  s.file = std::move(f);  // kept open; prefixRead() reads it on demand
  s.streamed = true;
  // Cache the font's lookup/metric prefix (cmap, loca, hmtx before glyf) in
  // PSRAM so each glyph load only needs one SD read. Read the font header
  // first to calculate the prefix size (capped at 1 MB) and allocate just that
  // amount of memory to to the prefix, only when there is enough free PSRAM
  static constexpr size_t kStreamPrefix = 1024 * 1024;
  const size_t prefix = computeTtfPrefixBytes(s.file, len, kStreamPrefix);
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > prefix + 256 * 1024) {
    s.bytes.resize(prefix);
    if (s.file.seek(0) && static_cast<size_t>(s.file.read(s.bytes.data(), prefix)) == prefix) {
      LOG_DBG("SDFS", "Cached %u KB TTF prefix in PSRAM", static_cast<unsigned>(prefix / 1024));
    } else {
      freeink::font::PsramVector<uint8_t>().swap(s.bytes);  // fall back to pure streaming
    }
  }
  LOG_DBG("SDFS", "Streaming TTF %s (%u KB) from SD", path.c_str(), static_cast<unsigned>(len / 1024));
  s.size = static_cast<unsigned long>(len);
  s.present = true;
  return true;
}

// Streamed-source read: serve from the PSRAM prefix cache when the range is
// there, hit SD only for the tail (glyf outlines). A read straddling the
// boundary splits across both.
unsigned long SdCardFontSystem::prefixRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                           const unsigned long count) {
  auto* s = static_cast<TtfSource*>(ctx);
  const auto readFromCard = [s](const unsigned long at, unsigned char* out, const unsigned long length) {
    const auto read = SdCardFontRegistry::halFileRead(&s->file, at, out, length);
    if (s->uiReadFailed && read < length && at <= s->size && length <= s->size - at) {
      s->uiReadFailed->store(true, std::memory_order_relaxed);
    }
    return read;
  };
  const unsigned long cached = s->bytes.size();
  if (offset < cached) {
    const unsigned long fromCache = (offset + count <= cached) ? count : cached - offset;
    if (count == 0) return 0;  // seek probe
    memcpy(buffer, s->bytes.data() + offset, fromCache);
    if (fromCache == count) return count;
    return fromCache + readFromCard(offset + fromCache, buffer + fromCache, count - fromCache);
  }
  return readFromCard(offset, buffer, count);
}

void SdCardFontSystem::addTtfSources(TtfEpdFont& font, TtfSource* sources) {
  for (uint8_t st = 0; st < 4; ++st) {
    TtfSource& s = sources[st];
    if (!s.present) continue;
    if (s.streamed) {
      font.addStreamSource(st, &SdCardFontSystem::prefixRead, &s, s.size);
    } else {
      font.addResidentSource(st, s.bytes.data(), static_cast<uint32_t>(s.bytes.size()));
    }
  }
}

bool SdCardFontSystem::openTtfFamilySources(const SdCardFontFamilyInfo& family, TtfSource* sources) {
  // A single-file family (loose .ttf, or a folder with one file) supplies only
  // regular; TtfEpdFont then derives bold/italic from the wght axis or an
  // oblique shear. Extra files upgrade those styles to the real designs.
  char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
  const uint32_t generation = registry_.scanGeneration();
  bool complete = true;
  for (const auto& file : family.files) {
    const uint8_t role = file.style < 4 ? file.style : 0;
    if (sources[role].present) continue;  // registry already deduped by role
    if (!family.filePath(file, path, sizeof(path))) {
      LOG_ERR("SDFS", "Vector font path too long in family %s", family.name.c_str());
      complete = false;
      continue;
    }
    if (!openTtfSource(sources[role], path)) complete = false;
  }
  sourceSlotGenerations_[sources == sourceSlots_[1]] = complete && sources[0].present ? generation : 0;
  return sources[0].present;
}

void SdCardFontSystem::unloadUiTtf(GfxRenderer& renderer) {
  if (uiTtf_[0]) {
    for (const auto& ui : kUiFontSizes) {
      renderer.unregisterTtfFont(ui.fontId);
      renderer.removeFont(ui.fontId);
    }
  }
  // Variants borrow the master's faces, and the master reads the sources.
  for (size_t i = std::size(uiTtf_); i-- > 0;) uiTtf_[i].reset();
  if (uiTtfSources_) {
    for (size_t i = 0; i < 4; ++i) uiTtfSources_[i].uiReadFailed = nullptr;
  }
  releaseTtfSources(uiTtfSources_, ttfSources_);
}

bool SdCardFontSystem::loadUiTtf(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  static_assert(std::size(kUiFontSizes) == sizeof(uiTtf_) / sizeof(uiTtf_[0]), "one face per UI font slot");
  if (family.name == ttfFamily_ && canShareTtfSources(ttfSources_)) {
    uiTtfSources_ = ttfSources_;
  } else {
    uiTtfSources_ = unusedTtfSources(ttfSources_);
    if (!openTtfFamilySources(family, uiTtfSources_)) return false;
  }
  for (size_t i = 0; i < 4; ++i) uiTtfSources_[i].uiReadFailed = &uiTtfFailed_;
  // A UI screen shows well under 512 distinct glyphs per size; the glyph
  // arenas are PSRAM-backed (FontPsram).
  static constexpr size_t kUiGlyphCacheBytes = 64 * 1024;
  static constexpr uint16_t kUiMaxGlyphs = 512;
  for (size_t i = 0; i < std::size(uiTtf_); ++i) {
    auto font = makeUniqueNoThrow<TtfEpdFont>();
    if (!font) return false;
    const uint8_t size = kUiFontSizes[i].pointSize;
    bool ok;
    if (i == 0) {
      addTtfSources(*font, uiTtfSources_);
      ok = font->load(size, /*twoBit=*/true, kUiGlyphCacheBytes, kUiMaxGlyphs);
    } else {
      ok = font->loadVariantOf(*uiTtf_[0], size, /*twoBit=*/true, kUiGlyphCacheBytes, kUiMaxGlyphs);
    }
    if (!ok) return false;
    font->setGlyphLoadFailureFlag(&uiTtfFailed_);
    if (!font->build(" ") || uiTtfFailed()) return false;
    uiTtf_[i] = std::move(font);
  }
  // All three sizes loaded: only now replace the slots, so a failure above
  // never leaves a slot without a font.
  for (size_t i = 0; i < std::size(uiTtf_); ++i) {
    const int id = kUiFontSizes[i].fontId;
    renderer.removeFont(id);
    auto font = uiTtf_[i]->family();
    font.setGlyphFallback(builtinUiFonts_[i]);
    font.setGlyphLoadFailureFlag(&uiTtfFailed_);
    renderer.insertFont(id, font);
    renderer.registerTtfFont(id, uiTtf_[i].get());
  }
  return true;
}

void SdCardFontSystem::recoverUiFont(GfxRenderer& renderer) {
  if (!uiTtfFailed()) return;
  LOG_ERR("SDFS", "UI glyph load failed in %s; using built-in fonts", activeUiFontName_);
  applyUiFont(renderer, "");
}

void SdCardFontSystem::clearTtfUiFallbacks(GfxRenderer& renderer) {
  if (ttfUiIds_.empty()) return;
  renderer.clearFallbackFonts();
  for (const int id : ttfUiIds_) {
    renderer.unregisterTtfFont(id);
    renderer.removeFont(id);
  }
  ttfUiIds_.clear();
  ttfUi_.clear();
}

void SdCardFontSystem::setupTtfUiFallbacks(GfxRenderer& renderer) {
  if (!ttf_ || activeUiFontName_[0] != '\0' || !ttfUiIds_.empty()) return;

  const uint32_t contentKey = ttf_->contentKey();
  // Small caches: UI strings (titles/rows) are short. Each UI family is a sized
  // variant of ttf_, sharing its FreeType faces without parsing font files or
  // allocating separate FT_Face objects in DRAM.
  static constexpr size_t kUiFallbackMinInternalHeap = 24 * 1024;
  for (const auto& ui : kUiFontSizes) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) == 0) {
      const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (internalFree < kUiFallbackMinInternalHeap) {
        LOG_DBG("SDFS", "Skipping TTF UI fallback @%upt (%u KB internal free)", ui.pointSize,
                static_cast<unsigned>(internalFree / 1024));
        continue;
      }
    }
    auto f = makeUniqueNoThrow<TtfEpdFont>();
    if (!f) {
      LOG_ERR("SDFS", "OOM: TtfEpdFont for UI fallback @%upt", ui.pointSize);
      continue;  // built-in bitmap UI fonts keep covering this size
    }
    const bool ok =
        f->loadVariantOf(*ttf_, ui.pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/16 * 1024, /*maxGlyphs=*/384);
    if (!ok) continue;
    LOG_DBG("SDFS", "TTF UI fallback @%upt loaded (heap free %u)", ui.pointSize, (unsigned)ESP.getFreeHeap());
    // Separate reader and UI identities even when their point sizes match.
    const int id = computeTtfFontId(ttfFamily_.c_str(), contentKey, ui.pointSize, /*uiFallback=*/true);
    renderer.insertFont(id, f->family());
    renderer.registerTtfFont(id, f.get());
    renderer.setFallbackFont(ui.fontId, id);
    ttfUiIds_.push_back(id);
    ttfUi_.push_back(std::move(f));
  }
}

void SdCardFontSystem::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                     const bool registryWasDirty, const bool setupFallbacks) {
  // Keep sizes inherited from other families within the selectable vector range.
  snapFontPointSizeTo(
      snapToNearestPointSize(VECTOR_READER_POINT_SIZES, std::size(VECTOR_READER_POINT_SIZES), SETTINGS.fontPointSize));
  const uint8_t size = SETTINGS.fontPointSize;

  // Already loaded, same family + size, and disk unchanged → nothing to do.
  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name && ttfPointSize_ == size) return;

  // Reader-face glyph-cache budget (used by both the resize fast path and the
  // full load below): the default 32 KB holds ~90 CJK glyphs, but a CJK page
  // uses 300+, so the cache flush-cycles mid-page and every page turn
  // re-rasterizes the whole page through streamed SD reads (multi-second
  // turns). The arenas are PSRAM-backed (FontPsram); 1 MB / 4096 glyphs holds
  // a whole Japanese novel's working set (~3000 unique kanji+kana at ~350 B
  // each), so the flush-everything ceiling is never hit and warm page turns
  // are pure cache hits. Without PSRAM keep the internal-DRAM-safe default.
  const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
  const size_t cacheBytes = havePsram ? 1024 * 1024 : 32 * 1024;
  const uint16_t maxGlyphs = havePsram ? 4096 : 768;

  // Same family, only the reader size changed (size preview): the open style
  // sources and the size-independent UI fallbacks don't need rebuilding — just
  // re-drive the reader face at the new size, reusing the already-open files
  // instead of reopening all four and rebuilding every UI fallback.
  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name) {
    clearTtfVariants(renderer);  // their ids derive from the old reader size
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);
    if (ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs)) {
      ttf_->build(" ");
      ttfFontId_ = computeTtfFontId(family.name.c_str(), ttf_->contentKey(), size);
      renderer.insertFont(ttfFontId_, ttf_->family());
      renderer.registerTtfFont(ttfFontId_, ttf_.get());
      ttfPointSize_ = size;
      return;
    }
    // Resize failed: fall through to a clean full reload.
  }

  unloadTtf(renderer);

  if (family.files.empty()) {
    LOG_ERR("SDFS", "Vector family %s has no file", family.name.c_str());
    SETTINGS.clearSdFontFamily();
    return;
  }

  const bool reuseUiSource = family.name == activeUiFontName_ && canShareTtfSources(uiTtfSources_);
  ttfSources_ = reuseUiSource ? uiTtfSources_ : unusedTtfSources(uiTtfSources_);
  if (!reuseUiSource && !openTtfFamilySources(family, ttfSources_)) {
    // Possibly a transient SD read failure: keep the user's selection so the
    // next ensureLoaded() retries; this session falls back to the built-in.
    LOG_ERR("SDFS", "Vector family %s: regular file failed to open (keeping selection)", family.name.c_str());
    releaseTtfSources(ttfSources_, uiTtfSources_);
    return;
  }

  ttf_ = makeUniqueNoThrow<TtfEpdFont>();
  if (!ttf_) {
    // Transient OOM: keep the user's selection (unlike a parse failure) so the
    // next ensureLoaded() can retry once heap pressure passes.
    LOG_ERR("SDFS", "OOM: TtfEpdFont for %s", family.name.c_str());
    releaseTtfSources(ttfSources_, uiTtfSources_);
    return;
  }
  addTtfSources(*ttf_, ttfSources_);
  const bool ok = ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs);
  if (!ok) {
    // init failure is ambiguous (corrupt font vs. transient OOM inside
    // FreeType): keep the selection and retry next ensureLoaded() rather than
    // silently reverting the user to the built-in font. A genuinely broken
    // font costs one failed load per reader entry, visible in the log.
    LOG_ERR("SDFS", "FreeInkFont could not parse %s (keeping selection)", family.name.c_str());
    ttf_.reset();
    releaseTtfSources(ttfSources_, uiTtfSources_);
    return;
  }
  // Seed the regular face's glyph cache; other styles + glyphs fault on demand.
  ttf_->build(" ");

  ttfFontId_ = computeTtfFontId(family.name.c_str(), ttf_->contentKey(), size);
  renderer.insertFont(ttfFontId_, ttf_->family());
  renderer.registerTtfFont(ttfFontId_, ttf_.get());
  ttfFamily_ = family.name;
  ttfPointSize_ = size;
  LOG_DBG("SDFS", "Reader TTF face loaded (heap free %u, max block %u)", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  if (setupFallbacks) setupTtfUiFallbacks(renderer);
  LOG_DBG("SDFS", "Loaded TTF font: %s @ %upt (id %d)", family.name.c_str(), size, ttfFontId_);
}

#endif  // CROSSPOINT_VECTOR_FONTS

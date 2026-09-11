#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>
#include <string>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Ensure the UI has a CJK fallback font if the active UI language needs one
  /// (e.g. zh-Hant), independent of whatever reading font is selected. Call
  /// after I18n::setLanguage() and once at boot. No-op if the language
  /// doesn't need CJK or if the reading font already provides it (see
  /// setupUiFallbacks). Returns false only when the language needs CJK and no
  /// source for it (reading font or installed SD family) is available —
  /// callers that just switched languages should treat that as "revert,
  /// this pick isn't usable yet" rather than leaving the UI half-legible.
  bool ensureUiLanguageFallback(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  // Load/unload the reading-font family per SETTINGS.sdFontFamilyName and
  // point size, re-discovering the registry first if it was marked dirty.
  void ensureReadingFont(GfxRenderer& renderer);

  // Load `family`'s UI point sizes into `mgr` and register each as the
  // fallback for the matching built-in UI font (see kUiFontSizes), so CJK
  // text renders at the same size as the surrounding Latin UI text. Safe to
  // call repeatedly: loadFamilyExtraSize() reuses an already-loaded size.
  // Returns false if any size is missing (a partial family shouldn't be
  // treated as covering the UI).
  static bool loadUiFallbackSizes(SdCardFontManager& mgr, const SdCardFontFamilyInfo& family, GfxRenderer& renderer);

  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched CJK fallback for the corresponding UI font, so CJK book
  // titles/list rows render at the same size as the surrounding Latin UI text.
  // No-op when no SD family is loaded. Safe to call repeatedly (sizes already
  // loaded are reused).
  void setupUiFallbacks(GfxRenderer& renderer);

  // True if the currently loaded reading font family (if any) has CJK
  // coverage, per the same representative-codepoint probe setupUiFallbacks
  // uses. Shared so ensureUiLanguageFallback() can avoid double-loading a
  // CJK family the reading font already provides.
  bool readingFontHasCjkCoverage(const GfxRenderer& renderer) const;

  // Stricter version for ensureUiLanguageFallback()'s own decision: requires
  // actual Han coverage, not just any single CJK script. See its definition.
  bool readingFontCoversZhHant(const GfxRenderer& renderer) const;

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  // Independent from `manager_`: holds a CJK family loaded purely to back
  // zh-Hant menu text, so it survives the user picking/clearing a reading
  // font that has no CJK coverage. See ensureUiLanguageFallback().
  SdCardFontManager uiLangFallbackManager_;
  std::string uiLangFallbackFamily_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;

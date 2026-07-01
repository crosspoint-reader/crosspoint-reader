#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

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

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Access the standalone drop-cap font registry (for the settings UI to
  /// enumerate the fonts installed under /.dropcap).
  const SdCardFontRegistry& dropCapRegistry() const { return dropCapRegistry_; }

  /// Load a specific drop-cap family for live preview, ignoring the saved
  /// selection and the dropCapsEnabled toggle so the font-picker can show the
  /// glyphs even with drop caps off. Returns the font id (0 for empty/not found).
  /// Call ensureLoaded() afterwards to restore the real, toggle-aware state.
  int previewDropCap(GfxRenderer& renderer, const char* familyName) { return loadDropCapFamily(renderer, familyName); }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
      dropCapRegistry_.discoverDropCaps();
    }
  }

 private:
  // Keep the selected drop-cap face loaded in step with the reader's size
  // setting. The face is chosen by SETTINGS.dropCapFontName from the standalone
  // /.dropcap registry (independent of the body-font selection) and loaded into a
  // dedicated manager so the enlarged chapter initial renders from a real large
  // glyph instead of an integer-scaled body glyph. No-op when drop caps are
  // disabled, no font is selected, or the font isn't on the card.
  void ensureDropCapLoaded(GfxRenderer& renderer);

  // Load the named drop-cap family into dropCapManager_ at the reader's ordinal
  // size and publish its id on the renderer (0 when name is empty/not found).
  // Shared by ensureDropCapLoaded (toggle-aware) and previewDropCap (direct).
  int loadDropCapFamily(GfxRenderer& renderer, const char* familyName);

  SdCardFontRegistry registry_;
  SdCardFontRegistry dropCapRegistry_;
  SdCardFontManager manager_;
  SdCardFontManager dropCapManager_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;

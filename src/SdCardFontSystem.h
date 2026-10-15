#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <functional>

#include "ReaderFontSizes.h"

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts only. Call once during setup.
  ///
  /// SD font payloads are loaded lazily on reader entry (ensureLoaded*) and
  /// should be unloaded when leaving reader activities.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  void ensureLoaded(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for an explicit family + size.
  /// Used when the reader type determines which settings field to consult.
  /// onColdLoad (if set) fires only when the font has to be written to the flash
  /// partition (genuine first load) — callers use it to show a "loading font" popup.
  void ensureLoaded(GfxRenderer& renderer, const char* familyName, uint8_t pointSize,
                    const std::function<void()>& onColdLoad = {}, FlashCachePolicy policy = FlashCachePolicy::ReadWrite,
                    BuildArena* arena = nullptr);

  /// Load a family only to show it on screen. A family already present in the
  /// flash partition is still mmap'd from there; anything else is read straight
  /// from SD and the partition is left untouched. The font selection list uses
  /// this so moving the cursor cannot erase and rewrite the partition per row.
  void ensureLoadedForPreview(GfxRenderer& renderer, const char* familyName, uint8_t pointSize,
                              BuildArena* arena = nullptr) {
    ensureLoaded(renderer, familyName, pointSize, {}, FlashCachePolicy::ReadOnly, arena);
  }

  /// The sizes the reader offers for a family; "" or a name not on the card gives the built-in
  /// ladder, which is what the reader falls back to. Every reader of a stored size snaps it
  /// through this list (see ReaderSizeList).
  ReaderSizeList sizeListFor(const char* familyName) const;

  /// Resolve an SD card font ID from family name + stored point size: the ID that renders that
  /// family at sizeListFor(familyName).snap(pointSize), whether the face itself or its scaled
  /// alias. Returns 0 if the family is not loaded or that size was not prepared by
  /// ensureLoaded(). Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Unload any currently loaded SD font family, freeing its heap (intervals,
  /// kern/ligature tables, glyph cache — typically 24-60KB). The registry is
  /// preserved so the font can be reloaded on next ensureLoaded() call.
  void unload(GfxRenderer& renderer) { manager_.unloadAll(renderer); }
  // A TTF body font's native sizes, for the reader's size ladder (FontSizeLadder::setNative).
  std::optional<SdCardFontManager::NativeSizes> ttfNativeSizes(const int bodyFontId) const {
    return manager_.ttfNativeSizes(bodyFontId);
  }

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  SdCardFontRegistry& registry() { return registry_; }
  const SdCardFontRegistry& registry() const { return registry_; }

 private:
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
};

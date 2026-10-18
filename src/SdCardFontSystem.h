#pragma once

#include <Epub/BookFonts.h>
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <functional>
#include <string>

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
  void unload(GfxRenderer& renderer);
  // A TTF body font's native sizes, for the reader's size ladder (FontSizeLadder::setNative).
  std::optional<SdCardFontManager::NativeSizes> ttfNativeSizes(const int bodyFontId) const {
    return manager_.ttfNativeSizes(bodyFontId);
  }

  /// Publisher Fonts in `mode` (CrossPointSettings::PUBLISHER_FONTS) for the book cached at `cachePath`: a family
  /// BookFonts copied out of it becomes the book family, drawn like a family on the card while that book is open.
  /// Body Text and All Text take its body family; Accents, and All Text for a book without one, its accent family,
  /// when `accentAllowed` (the reader's font is built in: the font area holds one family, so an accent family beside
  /// a reader font on the card waits for a store of several). An empty `cachePath` forgets it. True when there is a
  /// book family to load (selectedFamilyName).
  bool selectBookFamily(const std::string& cachePath, uint8_t mode = 0, bool accentAllowed = false);
  /// The book family's name ("" when none), for ensureLoaded() and resolveFontId(). It starts with "book:",
  /// which no family directory can, so it never shadows a family on the card.
  const char* selectedFamilyName() const { return bookFamily_.name.c_str(); }
  /// The same while it is the book's body family, the reader's font for the book; "" for an accent family.
  const char* bookFamilyName() const { return bookFamilyIsAccent_ ? "" : selectedFamilyName(); }
  /// The book's body family name when it belongs to the book cached at `cachePath`, else "".
  const char* bookFamilyNameFor(const std::string& cachePath) const {
    return cachePath == bookCachePath_ ? bookFamilyName() : "";
  }
  /// The book's accent family name, when it is the book family of the book cached at `cachePath`, else "".
  const char* accentFamilyNameFor(const std::string& cachePath) const {
    return cachePath == bookCachePath_ && bookFamilyIsAccent_ ? selectedFamilyName() : "";
  }
  /// The families BookFonts found in the book cached at `cachePath`, while it is selected in a mode that lays its
  /// blocks out in them (Accents, All Text); else nullptr.
  const BookFonts::Families* bookFontFamiliesFor(const std::string& cachePath) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  SdCardFontRegistry& registry() { return registry_; }
  const SdCardFontRegistry& registry() const { return registry_; }

 private:
  // A family on the card, or the book family.
  const SdCardFontFamilyInfo* findFamily(const char* name) const;

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  SdCardFontFamilyInfo bookFamily_;  // empty name: none
  std::string bookCachePath_;        // the book bookFamily_ was selected for
  BookFonts::Families bookFonts_;    // what that book holds
  uint8_t bookMode_ = 0;             // the mode it was selected in
  bool bookAccentAllowed_ = false;
  bool bookFamilyIsAccent_ = false;  // bookFamily_ is the book's accent family, not its body's
};

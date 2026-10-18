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
  /// begin() has run: the SD families are known. The boot straight into USB Transfer skips it.
  bool discovered() const { return discovered_; }
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
                    BuildArena* arena = nullptr) {
    ensureLoaded(renderer, &familyName, 1, pointSize, onColdLoad, policy, arena);
  }
  /// The same for families drawn together, the body's first ("" or a name not on the card is left out): TrueType
  /// families share one glyph store (SdCardFontManager::loadTtfFamilies); a .cpfont family first is loaded alone.
  /// Kept as loaded only while they are the same families from the same files.
  void ensureLoaded(GfxRenderer& renderer, const char* const* familyNames, uint8_t count, uint8_t pointSize,
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

  /// Publisher Fonts in `mode` (CrossPointSettings::PUBLISHER_FONTS) for the book cached at `cachePath`, whose body
  /// is otherwise drawn in `readerFamily` ("" for a built-in font): the families BookFonts copied out of it become
  /// the book's, drawn like families on the card while that book is open. Body Text and All Text take its body family
  /// as the book's font; Accents and All Text add its accent family beside whatever draws the body, a built-in font
  /// or a TrueType family, which then share the glyph store. Beside a .cpfont family, which has the font area to
  /// itself, the accent family is left out. An empty `cachePath` forgets them.
  void selectBookFamilies(const std::string& cachePath, uint8_t mode = 0, const char* readerFamily = "");
  /// The book's body family's name while it is the reader's font for the book, else "", for ensureLoaded() and
  /// resolveFontId(). A book family's name starts with "book:", which no family directory can, so it never shadows
  /// a family on the card.
  const char* bookFamilyName() const { return bookBody_.name.c_str(); }
  /// The book's accent family's name while it is selected, else "".
  const char* accentFamilyName() const { return bookAccent_.name.c_str(); }
  /// The book's body family name when it belongs to the book cached at `cachePath`, else "".
  const char* bookFamilyNameFor(const std::string& cachePath) const {
    return cachePath == bookCachePath_ ? bookFamilyName() : "";
  }
  /// The book's accent family name, when it belongs to the book cached at `cachePath`, else "".
  const char* accentFamilyNameFor(const std::string& cachePath) const {
    return cachePath == bookCachePath_ ? accentFamilyName() : "";
  }
  /// The families BookFonts found in the book cached at `cachePath`, while it is selected in a mode that lays its
  /// blocks out in them (Accents, All Text); else nullptr.
  const BookFonts::Families* bookFontFamiliesFor(const std::string& cachePath) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  SdCardFontRegistry& registry() { return registry_; }
  const SdCardFontRegistry& registry() const { return registry_; }

 private:
  bool discovered_ = false;
  // A family on the card, or one of the book's.
  const SdCardFontFamilyInfo* findFamily(const char* name) const;
  // Reads a book family's directory as a family directory on the card, named "book:<name>"; else empties `family`.
  void scanBookFamily(SdCardFontFamilyInfo& family, const std::string& name, const std::string& dir);

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  SdCardFontFamilyInfo bookBody_;    // the book's body family; empty name: none
  SdCardFontFamilyInfo bookAccent_;  // the book's accent family; empty name: none
  std::string bookCachePath_;        // the book they were selected for
  BookFonts::Families bookFonts_;    // what that book holds
  uint8_t bookMode_ = 0;             // the mode they were selected in
  bool bookBesideCpfont_ = false;    // the reader's font was a .cpfont family then
};

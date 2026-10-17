#pragma once

#include "SdCardFontSystem.h"

class GfxRenderer;

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;

// Ensure the correct SD card font family is loaded for current settings.
// Defined in main.cpp; call before entering the reader or after settings change.
extern void ensureSdFontLoaded();

// Ensure the correct SD card font family is loaded for the given book path.
// Selects EPUB or TXT/MD settings depending on the file extension.
// Defined in main.cpp.
// Publisher Fonts: an EPUB whose embedded fonts have not been examined yet (bookFontsPendingForPath) keeps the
// font that is loaded while waitForBookFonts holds, so the family the setting replaces is not installed first;
// the reader examines them while the book opens and calls again with waitForBookFonts false.
extern void ensureSdFontLoadedForPath(const char* path, bool waitForBookFonts = true);

// True when the EPUB at `path` is to be drawn in its embedded body font (Publisher Fonts on, the book's own
// style in use, no font family chosen for it) and BookFonts has not examined it yet. Defined in main.cpp.
extern bool bookFontsPendingForPath(const char* path);

// Unload any loaded SD font payload (heap reclamation). Safe no-op when none
// is loaded. Defined in main.cpp.
extern void unloadSdFontIfLoaded();

// Resolve the SD card font ID for the given family name and stored point size.
// Returns 0 if no SD font with that family name and size is currently loaded.
// Free function (not stored as a callback in CrossPointSettings) so the linker
// can resolve it directly without runtime indirection.
int resolveSdCardFontId(const char* familyName, uint8_t pointSize);

// Trampolines used by the dynamic font-family SettingInfo. They walk
// sdFontSystem's registry on each call to translate between
// (built-in index | built-in count + SD index) and the appropriate
// CrossPointSettings field (fontFamily vs sdFontFamilyName).
// Signatures match SettingInfo::ValueGetterFn / ValueSetterFn so they can
// be wired directly into a DynamicEnum SettingInfo without further indirection.
uint8_t fontFamilyDynamicGetter(const void* ctx);
void fontFamilyDynamicSetter(void* ctx, uint8_t value);

// Same as above but for the TXT/MD reader font family setting.
uint8_t txtFontFamilyDynamicGetter(const void* ctx);
void txtFontFamilyDynamicSetter(void* ctx, uint8_t value);

// Returns the total number of font-family options currently available
// (BUILTIN_FONT_COUNT + number of discovered SD families). Used by the
// settings UI / web layer to enrich enumLabels and to bound cycling.
uint8_t fontFamilyOptionCount();

// Returns the localized label for option index `i`. For built-in indices
// (< BUILTIN_FONT_COUNT) this returns the I18N string; for SD indices it
// returns the family name from sdFontSystem.registry().
#include <string>
std::string fontFamilyOptionLabel(uint8_t i);

// Trampolines for the reader font-size rows (EPUB, TXT/MD). The rows store a point size and the
// settings UI deals in option indices; both directions go through the size list of the family
// the row belongs to (SdCardFontSystem::sizeListFor).
uint8_t fontSizeDynamicGetter(const void* ctx);
void fontSizeDynamicSetter(void* ctx, uint8_t index);
uint8_t txtFontSizeDynamicGetter(const void* ctx);
void txtFontSizeDynamicSetter(void* ctx, uint8_t index);

// Options for the same rows: the sizes the selected family offers, read when asked, so the list
// follows a family chosen while the screen is open.
uint8_t fontSizeOptionCount(const void* ctx);
std::string fontSizeOptionLabel(const void* ctx, uint8_t index);
uint8_t txtFontSizeOptionCount(const void* ctx);
std::string txtFontSizeOptionLabel(const void* ctx, uint8_t index);

// The SD family a book is drawn with, given its overrides: "" when it overrides to a built-in
// family, its own SD override when it has one, otherwise the global choice. Points into
// `sdFamilyOverride` or SETTINGS, so use it before either changes.
const char* bookSdFontFamily(int8_t builtinFamilyOverride, const std::string& sdFamilyOverride);

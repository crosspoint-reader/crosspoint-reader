#pragma once

#include "SdCardFontSystem.h"

class GfxRenderer;

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;

// Ensure the correct SD card font family is loaded for current settings.
// Defined in main.cpp; call before entering the reader or after settings change.
extern void ensureSdFontLoaded();

// Resolve the SD card font ID for the given family name.
// Returns 0 if no SD font with that family name is currently loaded.
// Free function (not stored as a callback in CrossPointSettings) so the linker
// can resolve it directly without runtime indirection.
int resolveSdCardFontId(const char* familyName);

// Trampolines used by the dynamic font-family SettingInfo. They walk
// sdFontSystem's registry on each call to translate between
// (built-in index | built-in count + SD index) and the appropriate
// CrossPointSettings field (fontFamily vs sdFontFamilyName).
// Signatures match SettingInfo::ValueGetterFn / ValueSetterFn so they can
// be wired directly into a DynamicEnum SettingInfo without further indirection.
uint8_t fontFamilyDynamicGetter(const void* ctx);
void fontFamilyDynamicSetter(void* ctx, uint8_t value);

// Returns the total number of font-family options currently available
// (BUILTIN_FONT_COUNT + number of discovered SD families). Used by the
// settings UI / web layer to enrich enumLabels and to bound cycling.
uint8_t fontFamilyOptionCount();

// Returns the localized label for option index `i`. For built-in indices
// (< BUILTIN_FONT_COUNT) this returns the I18N string; for SD indices it
// returns the family name from sdFontSystem.registry().
#include <string>
std::string fontFamilyOptionLabel(uint8_t i);

#pragma once

#include "SdCardFontSystem.h"

class GfxRenderer;

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;

// Ensure the correct SD card font family is loaded for current settings.
// Defined in main.cpp; call before entering the reader or after settings change.
extern void ensureSdFontLoaded();

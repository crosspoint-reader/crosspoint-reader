# Comparison with Upstream (crosspoint-reader/master)

This document details the modifications and enhancements made in this fork/branch compared to the upstream master branch (`https://github.com/crosspoint-reader/crosspoint-reader`).

## 1. Custom Font Support Framework

A complete system for loading and rendering custom fonts from the SD card was implemented.

### New Core Components
*   **`lib/EpdFontLoader/EpdFontLoader.{cpp,h}`**: A new library responsible for discovering, validating, and ensuring custom fonts are loaded into the renderer. It includes safety fallbacks to the default "Bookerly" font if a custom font fails to load.
*   **`src/managers/FontManager.{cpp,h}`**: A singleton manager that scans the `/fonts` directory on the SD card, parses `.epdfont` headers, and creates `EpdFontFamily` instances for valid fonts.
*   **`lib/EpdFont/CustomEpdFont.{cpp,h}`**: A new font implementation that reads glyph data directly from the binary `.epdfont` files on the SD card, implementing an LRU cache for bitmaps to optimize RAM usage.
*   **`src/activities/settings/FontSelectionActivity.{cpp,h}`**: A new UI screen allowing the user to select from available custom fonts found on the SD card.
*   **`lib/EpdFont/EpdFontStyles.h`**: Added to define styles (Regular, Bold, Italic, BoldItalic) for better font family management.

### Tooling Updates
*   **`lib/EpdFont/scripts/fontconvert.py`**: Significantly rewritten to generate binary `.epdfont` files with a specific 48-byte header and 13-byte glyph structures required by the new firmware reader. It fixes offset calculations that were broken in the original version.

## 2. EPUB Rendering & Parsing Improvements

The EPUB reader core was modified to improve stability, performance, and memory management.

*   **`lib/Epub/Epub/Section.cpp`**: 
    *   Removed `SDLock` usage which was causing compilation issues.
    *   Cleaned up file I/O operations and caching logic.
*   **`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`**: 
    *   Removed `SDLock` dependency.
    *   Integrated better progress reporting and memory monitoring logs.
*   **`lib/Epub/Epub.cpp`**: Enhanced error handling during book loading.
*   **`lib/Epub/Epub/Page.cpp`**: Optimized page serialization/deserialization.

## 3. Graphics Renderer Enhancements

*   **`lib/GfxRenderer/GfxRenderer.{cpp,h}`**:
    *   Updated to support `CustomEpdFont` alongside built-in compiled headers.
    *   Implemented font ID based lookup that seamlessly handles both built-in and dynamic custom fonts.
    *   Removed excessive verbose logging to improve performance in production builds.

## 4. Application State & Settings

*   **`src/CrossPointSettings.{cpp,h}`**: 
    *   Added persistent storage for the selected `customFontFamily`.
    *   Updated `getReaderFontId()` to resolve IDs dynamically via `EpdFontLoader` when a custom font is selected.
*   **`src/main.cpp`**: 
    *   **CRITICAL FIX**: Re-enabled `verifyWakeupLongPress()` to prevent the device from accidentally powering on when plugged in or bumped.
    *   Integrated `EpdFontLoader::loadFontsFromSd` into the startup sequence.

## 5. User Interface Updates

*   **`src/activities/settings/SettingsActivity.cpp`**: Added the "Reader Font Family" menu option to navigate to the new font selection screen.
*   **`src/activities/reader/EpubReaderActivity.cpp`**: Updated to use the dynamic font loading system and respect the user's custom font choice.

## 6. Documentation

*   **`CUSTOM_FONTS.md`**: Created detailed developer documentation explaining the architecture of the custom font system.
*   **`FONT_CONVERSION.md`**: Added a user guide for converting `.ttf`/`.otf` files to `.epdfont` using the Python script.
*   **`USER_GUIDE.md`**: Updated with a new section on Custom Fonts and how to use them.

## Summary of Files Added/Modified

**New Files:**
*   `CUSTOM_FONTS.md`
*   `FONT_CONVERSION.md`
*   `lib/EpdFont/CustomEpdFont.{cpp,h}`
*   `lib/EpdFont/EpdFontStyles.h`
*   `lib/EpdFontLoader/EpdFontLoader.{cpp,h}`
*   `src/activities/settings/FontSelectionActivity.{cpp,h}`
*   `src/managers/FontManager.{cpp,h}`

**Modified Files:**
*   Core Logic: `src/main.cpp`, `src/CrossPointSettings.{cpp,h}`, `src/CrossPointState.cpp`
*   UI: `src/activities/settings/SettingsActivity.{cpp,h}`, `src/activities/reader/EpubReaderActivity.cpp`, `src/activities/reader/FileSelectionActivity.cpp`
*   Rendering: `lib/GfxRenderer/GfxRenderer.{cpp,h}`, `lib/EpdFont/EpdFont.{cpp,h}`
*   EPUB Engine: `lib/Epub/*` (various files optimized and cleaned)
*   Tools: `lib/EpdFont/scripts/fontconvert.py`

### Update: Enhanced Font Discovery & Format Support (2025-01-20)

*   **V1 Format Support**: Added full support for the newer V1 `.epdfont` format (32-byte header, uint32 offsets) used by the web-based converter (`epdfont.clev.app`).
*   **V0 Format Fix**: Fixed a regression in V0 font loading where the header read was truncated to 32 bytes (instead of 48), restoring support for `LibreBaskerville` and other legacy fonts.
*   **Flexible Discovery**: Updated `FontManager` to support `Family_Style_Size` (underscore-separated) naming conventions, enabling compatibility with a wider range of auto-generated filenames.
*   **Documentation**: Rewrote `FONT_CONVERSION.md` to cover both the Python script and the new web converter.

### Update: Typographic Rendering Improvements (2026-01-22)

*   **Precise Character Spacing**: Implemented `getTextAdvance` to use typographic advance widths instead of visual bounding boxes for layout. This fixes clipping issues with characters like em-dashes.
*   **Punctuation Attachment**: Added logic to `ParsedText` to "attach" punctuation (., ,, ;, etc.) to the preceding word, ensuring no visual gap appears between the word and the punctuation mark, even when line breaking occurs.
*   **Font Converter Precision**: Updated `lib/EpdFont/scripts/fontconvert.py` to use rounding instead of flooring for advance width calculations and fixed a binary file writing bug, resulting in higher quality generated fonts.

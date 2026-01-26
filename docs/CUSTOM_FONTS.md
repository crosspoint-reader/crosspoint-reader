
# Custom Font Implementation Walkthrough

This document outlines the custom font implementation in the CrossPoint Reader codebase. The system allows users to load custom TrueType/OpenType fonts (converted to a binary format) from an SD card and select them via the settings UI.

## System Overview

The custom font system consists of four main components:
1.  **Font Converter (`fontconvert.py`)**: A Python script that pre-processes standard fonts into a custom optimized binary format (`.epdfont`).
2.  **Font Manager (`FontManager`)**: Scans the SD card for valid font files and manages loaded font families.
3.  **Font Loader (`CustomEpdFont`)**: Handles the low-level reading of the binary format, including on-demand caching of glyph bitmaps to save RAM.
4.  **UI & Integration**: A settings activity to select fonts and integration into the main rendering loop.

## 1. Font Conversion & Format

To optimize for the limited RAM of the ESP32 and the specific requirements of E-Ink displays, fonts are not loaded directly as TTF/OTF files. Instead, they are pre-processed.

*   **Script**: `lib/EpdFont/scripts/fontconvert.py`
*   **Input**: TTF/OTF files.
*   **Output**: `.epdfont` binary file.
*   **Format Details**:
    *   **Header**:
        *   **Version 1 (New)**: 32-byte header, uint32 offsets. Compact and efficient.
        *   **Version 0 (Legacy)**: 48-byte header, uint16 offsets. Retained for backward compatibility.
    *   **Intervals**: Unicode ranges supported by the font.
    *   **Glyphs**: Metrics for each character (width, height, advance, offsets).
    *   **Bitmaps**: 1-bit or 2-bit (antialiased) pixel data for glyphs.

## 2. Storage & Discovery

Fonts are stored on the SD card in the `/fonts` directory.

*   **Location**: `/fonts`
*   **Naming Convention**:
    *   **Standard**: `Family-Style-Size.epdfont` (e.g., `LibreBaskerville-Regular-14.epdfont`)
    *   **Web Converter**: `Family_Style_Size.epdfont` (e.g., `Aileron_Regular_18.epdfont`)
    *   **Single File**: `Family.epdfont` (e.g., `Aileron.epdfont`) - automatically detected as Regular style.
*   **Manager**: `src/managers/FontManager.cpp`
    *   **Scans** the `/fonts` directory on startup/demand.
    *   **Groups** files into `Family -> Size -> Styles (Regular, Bold, Italic, BoldItalic)`.
    *   Exposes available families to the UI.

## 3. Low-Level Implementation (RAM Optimization)

The core logic resides in `lib/EpdFont/CustomEpdFont.cpp`.

*   **Inheritance**: `CustomEpdFont` inherits from `EpdFont`.
*   **Metadata in RAM**: When a font is loaded, only the *header* and *glyph metrics* (width, height, etc.) are loaded into RAM.
*   **Bitmaps on Disk**: Pixel data remains on the SD card.
*   **LRU Cache**: A small Least Recently Used (LRU) cache (`MAX_CACHE_SIZE = 30`) holds frequently used glyph bitmaps in RAM.
    *   **Hit**: Returns cached bitmap.
    *   **Miss**: Reads the bitmap from the SD card at the specific offset, caches it, and returns it.
*   **Benefit**: Allows using large fonts with extensive character sets (e.g., CJK) without exhausting the ESP32's heap.

## 4. User Interface & Selection

The user selects a font through a dedicated Settings activity.

*   **File**: `src/activities/settings/CustomFontSelectionActivity.cpp`
*   **Flow**:
    1.  Lists available font families retrieved from `FontManager`.
    2.  User selects a family.
    3.  Selection is saved to `SETTINGS.customFontFamilyName`.

## 5. Main Integration

The selected font is applied during the system startup or when settings change.

*   **File**: `src/main.cpp`
*   **Function**: `setupDisplayAndFonts()`
*   **Logic**:
    1.  Checks if `SETTINGS.fontFamily` is set to `FONT_CUSTOM`.
    2.  Calls `FontManager::getInstance().getCustomFontFamily(...)` with the saved name and current font size.
    3.  If found, the font is dynamically inserted into the global `renderer` with a generated ID.
    4.  The renderer then uses this font for standard text rendering.

## Code Path Summary

1.  **SD Card**: `SD:/fonts/MyFont-Regular-14.epdfont`
2.  **Wait**: `FontManager::scanFonts()` finds the file.
3.  **Select**: User picks "MyFont" in `CustomFontSelectionActivity`.
4.  **Load**: `main.cpp` calls `renderer.insertFont(..., FontManager.getCustomFontFamily("MyFont", 14))`
5.  **Render**: `CustomEpdFont::getGlyphBitmap()` fetches pixels from SD -> Cache -> Screen.

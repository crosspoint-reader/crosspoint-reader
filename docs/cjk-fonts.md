# CJK Font Support

This guide explains how to use Chinese, Japanese, and Korean (CJK) fonts on your CrossPoint Reader.

## Overview

CrossPoint Reader supports external CJK fonts for both:

- **UI Font** - Used for menus, settings, and system interface
- **Reader Font** - Used for reading ebook content

The device includes a built-in CJK UI font (Source Han Sans subset) for basic interface rendering, and supports loading custom external fonts from the SD card.

## Prerequisites

- CrossPoint Reader device with firmware version supporting CJK fonts
- SD card with available space for font files
- TrueType font files (.ttf) converted to the CrossPoint font format

---

## Font File Format

CrossPoint Reader uses a custom binary font format optimized for e-ink displays. Font files must be placed in the `/fonts/` directory on the SD card.

### File Naming Convention

Font files must follow this naming pattern:

```
{FontName}_{Size}_{Width}x{Height}.bin
```

**Examples:**
- `SourceHanSansCN-Medium_20_20x20.bin`
- `KingHwaOldSong_38_33x39.bin`
- `Yozai-Medium_36_31x31.bin`

### Font File Structure

The binary font file contains:

1. **Header** (variable length)
   - Font name (null-terminated string)
   - Font size (uint8_t)
   - Character width (uint8_t)
   - Character height (uint8_t)
   - Bytes per character (uint16_t)

2. **Character Data**
   - Sequential bitmap data for each character
   - Characters are stored in Unicode order
   - Each character uses `width * height / 8` bytes (1-bit per pixel)

---

## Generating Font Files

Use the provided Python script to convert TrueType fonts:

```bash
python scripts/generate_cjk_ui_font.py \
    --font /path/to/font.ttf \
    --size 20 \
    --output /path/to/output.bin
```

### Script Options

| Option | Description |
|--------|-------------|
| `--font` | Path to TrueType font file (.ttf) |
| `--size` | Font size in points |
| `--output` | Output binary file path |
| `--chars` | (Optional) Custom character set file |

### Character Set

By default, the script generates fonts for:
- Basic ASCII characters (0x20-0x7E)
- Common CJK characters (GB2312 subset)
- Japanese Hiragana and Katakana
- Common punctuation marks

---

## Installing Fonts

1. **Create the fonts directory** (if it doesn't exist):
   ```
   /fonts/
   ```

2. **Copy font files** to the `/fonts/` directory on your SD card

3. **Restart the device** or go to Settings to scan for new fonts

---

## Selecting Fonts

### UI Font

1. Go to **Settings** → **Display**
2. Select **External UI Font**
3. Choose from available fonts or select **Built-in (Disabled)** to use the default font

### Reader Font

1. Go to **Settings** → **Reader**
2. Select **External Reader Font**
3. Choose from available fonts or select **Built-in (Disabled)** to use the default font

---

## Built-in CJK UI Font

The firmware includes a pre-rendered subset of Source Han Sans (思源黑体) for UI rendering. This font covers:

- All ASCII characters
- Common Chinese characters used in the UI
- Japanese Hiragana and Katakana
- Common punctuation marks

The built-in font is automatically used when:
- No external UI font is selected
- The external font file is missing or corrupted

---

## External UI Font Features

When an external UI font is selected:

### Full Character Coverage
- **All characters** (including ASCII letters, numbers, and punctuation) are rendered using the external font
- This ensures consistent visual style across the entire UI
- If a character is missing from the external font, the system falls back to built-in fonts

### Proportional Spacing
- External fonts use **proportional spacing** (variable width)
- Each character advances by its actual width, not a fixed width
- This makes English text look more natural with proper letter spacing
- CJK characters still use their full width as designed

---

## Recommended Fonts

### For UI (Small sizes, 18-24pt)

| Font | Description | License |
|------|-------------|---------|
| Source Han Sans | Clean, modern sans-serif | OFL |
| Noto Sans CJK | Google's CJK font family | OFL |
| WenQuanYi Micro Hei | Compact Chinese font | GPL |

### For Reading (Larger sizes, 28-40pt)

| Font | Description | License |
|------|-------------|---------|
| Source Han Serif | Traditional serif style | OFL |
| Noto Serif CJK | Google's serif CJK font | OFL |
| FangSong | Classic Chinese style | Varies |

---

## Memory Considerations

External fonts consume RAM when loaded. Consider these guidelines:

| Font Size | Approx. Memory Usage |
|-----------|---------------------|
| 20pt | ~50KB per 1000 characters |
| 28pt | ~100KB per 1000 characters |
| 36pt | ~160KB per 1000 characters |

**Tips:**
- Use smaller font sizes for UI (18-24pt)
- Larger fonts (32pt+) are better for reader content
- Only one UI font and one reader font are loaded at a time

---

## Troubleshooting

### Font not appearing in selection list

1. Check the file is in `/fonts/` directory
2. Verify the filename follows the naming convention
3. Ensure the file is not corrupted (try regenerating)

### Characters displaying as boxes or question marks

1. The character may not be included in the font
2. Try a font with broader character coverage
3. Check if the font file was generated correctly

### Device running slowly after selecting font

1. The font file may be too large
2. Try a smaller font size
3. Reduce the character set when generating the font

### Font looks blurry or pixelated

1. E-ink displays work best with specific font sizes
2. Try sizes that are multiples of the display's native resolution
3. Ensure anti-aliasing is disabled for 1-bit rendering

---

## Technical Details

### Font Manager API

The `FontManager` class provides:

```cpp
// Scan for available fonts
FontMgr.scanFonts();

// Get font count
int count = FontMgr.getFontCount();

// Get font info
const FontInfo* info = FontMgr.getFontInfo(index);

// Select reader font (-1 to disable)
FontMgr.selectFont(index);

// Select UI font (-1 to disable)
FontMgr.selectUiFont(index);

// Check if external font is enabled
bool enabled = FontMgr.isExternalFontEnabled();
```

### Font Info Structure

```cpp
struct FontInfo {
    char name[32];      // Font name
    uint8_t size;       // Font size in points
    uint8_t width;      // Character width in pixels
    uint8_t height;     // Character height in pixels
    uint16_t bytesPerChar; // Bytes per character
    char path[64];      // Full path to font file
};
```

---

## Related Documentation

- [Internationalization (I18N)](./i18n.md) - Multi-language support
- [File Formats](./file-formats.md) - Binary file format specifications
- [Troubleshooting](./troubleshooting.md) - General troubleshooting guide

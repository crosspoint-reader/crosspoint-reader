# Font Family Controls

The firmware now supports conditional compilation of font families to save flash memory.

## Current Flash Usage

With all 4 font families enabled:
- **Total flash**: 6,307,484 bytes (96.2% of 6.25 MB)
- **Fonts alone**: ~3-4 MB (60-65% of total)

## Available Font Families

### Reader Fonts (Can be disabled)

Each reader font family can be individually enabled or disabled:

### 1. **Bookerly** (`ENABLE_FONT_BOOKERLY`)
- Sizes: 12, 14, 16, 18 pt
- Styles: Regular, Bold, Italic, BoldItalic
- Estimated size: ~800 KB - 1 MB

### 2. **Noto Sans** (`ENABLE_FONT_NOTOSANS`)
- Sizes: 12, 14, 16, 18 pt (8pt is always included separately)
- Styles: Regular, Bold, Italic, BoldItalic
- Estimated size: ~800 KB - 1 MB

### 3. **OpenDyslexic** (`ENABLE_FONT_OPENDYSLEXIC`)
- Sizes: 8, 10, 12, 14 pt
- Styles: Regular, Bold, Italic, BoldItalic
- Estimated size: ~1-1.5 MB

### UI Fonts (Always Included)

**Ubuntu** - Used for menus and UI elements
- Sizes: 10, 12 pt
- Styles: Regular, Bold
- Estimated size: ~100-200 KB
- **Always included** - cannot be disabled

**NotoSans 8pt** - Small UI font
- Size: 8 pt
- Style: Regular only
- Estimated size: ~50 KB
- **Always included** - cannot be disabled

## How to Disable Fonts

⚠️ **WARNING**: At least ONE reader font family must remain enabled (Bookerly, NotoSans, or OpenDyslexic). Disabling all three will cause build errors and the reader won't function.

Edit `platformio.ini` and comment out the fonts you don't need:

```ini
# Font family controls (comment out to disable and save flash)
# WARNING: At least ONE reader font must be enabled
# Note: Ubuntu UI fonts are always included
  -DENABLE_FONT_BOOKERLY=1
  -DENABLE_FONT_NOTOSANS=1
# -DENABLE_FONT_OPENDYSLEXIC=1   # Disabled - saves ~1.5 MB
```

## Example Configurations

### Minimal Configuration (Save ~1.8 MB)
Keep only Bookerly, disable others:
```ini
  -DENABLE_FONT_BOOKERLY=1
# -DENABLE_FONT_NOTOSANS=1
# -DENABLE_FONT_OPENDYSLEXIC=1
```
**Result**: Flash usage drops to ~68-73%

### Balanced Configuration (Save ~1.5 MB)
Keep Bookerly + NotoSans, disable OpenDyslexic:
```ini
  -DENABLE_FONT_BOOKERLY=1
  -DENABLE_FONT_NOTOSANS=1
# -DENABLE_FONT_OPENDYSLEXIC=1
```
**Result**: Flash usage drops to ~70-75%

### Accessibility-Focused (Save ~1.5 MB)
Keep only OpenDyslexic + NotoSans:
```ini
# -DENABLE_FONT_BOOKERLY=1
  -DENABLE_FONT_NOTOSANS=1
  -DENABLE_FONT_OPENDYSLEXIC=1
```
**Result**: Flash usage drops to ~70-75%

## Important Notes

1. ⚠️ **AT LEAST ONE READER FONT MUST BE ENABLED** - You cannot disable all three (Bookerly, NotoSans, OpenDyslexic) or the reader will not function
2. **UI fonts are always included** - Ubuntu (10pt, 12pt) and NotoSans 8pt (~150-250 KB total)
3. After changing font settings, do a **clean rebuild**:
   ```bash
   platformio run --target clean
   platformio run
   ```

## Settings Menu Impact

When fonts are disabled, they will automatically be removed from the "Font Family" dropdown in the Settings menu. If the currently selected font is disabled during compilation, the device will default to the first available font family.

## Build Verification

After disabling fonts, verify the flash savings:
```bash
platformio run
```

Look for the output line showing flash usage:
```
RAM:   [=         ]  12.3% (used 40072 bytes from 327680 bytes)
Flash: [========  ]  75.2% (used 4928484 bytes from 6553600 bytes)
```

## Troubleshooting

### Build Errors
- ⚠️ **Most common cause**: All reader fonts are disabled! At least one of Bookerly, NotoSans, or OpenDyslexic must be enabled
- Do a clean build: `platformio run --target clean`
- Check that at least one `-DENABLE_FONT_*` flag is uncommented in platformio.ini

### Missing Fonts in Settings
- This is expected behavior - disabled fonts won't appear in the menu
- Check your platformio.ini configuration

### Font Falls Back to Wrong Family
- The device will use the first available font if the saved preference is disabled
- Go to Settings > Font Family and select an available font

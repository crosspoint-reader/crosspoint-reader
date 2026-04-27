# SD Card Fonts

CrossPoint supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your CrossPoint reader to WiFi
2. Go to **Settings > System > Download Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

### Option 2: Upload via web browser

1. Connect your CrossPoint reader to WiFi
2. Open the web interface in your browser (shown on the WiFi screen)
3. Navigate to the **Fonts** tab
4. Upload `.cpfont` files using the upload form

### Option 3: Manual SD card copy

1. Download font files from the
   [Releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases/tag/sd-fonts)
2. Copy font family folders to `/.crosspoint/fonts/` on your SD card:

       SD Card Root/
       └── .crosspoint/
           └── fonts/
               ├── Bookerly-SD/
               │   ├── Bookerly-SD_12.cpfont
               │   ├── Bookerly-SD_14.cpfont
               │   ├── Bookerly-SD_16.cpfont
               │   └── Bookerly-SD_18.cpfont
               └── ...

3. Insert the SD card and power on your CrossPoint reader

## Available Pre-Built Fonts

| Font | Best For | Languages |
|------|----------|-----------|
| Bookerly-SD | General reading | English, Western European |
| NotoSansExtended | Multi-script reading | European, Greek, Cyrillic, Georgian, Armenian, Ethiopic |
| NotoSansCJK | Chinese/Japanese/Korean | CJK + ASCII |

## Converting Custom Fonts

To convert your own TrueType/OpenType fonts:

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### Available Unicode interval presets

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020-U+007E (Basic Latin) |
| `latin-ext` | European languages (Latin + Extended-A/B) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables |
| `builtin` | Matches built-in Bookerly coverage exactly |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

Install custom fonts via WiFi upload or manual SD card copy.

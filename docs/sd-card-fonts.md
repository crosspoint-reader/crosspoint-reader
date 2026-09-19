# Fonts on the SD card

You can add fonts to the reader with its SD card. This lets you choose a different look and read languages that the built-in fonts do not cover.

## Add a font

Choose one of these methods:

### Download on the reader

1. Connect the reader to Wi-Fi.
2. Open **Settings → Reader → Manage Fonts**.
3. Select a font family to download.
4. Select the font in **Settings → Reader → Font Family**.

### Upload from a browser

1. Start **File Transfer** and select **Join Network** or **Create Hotspot**.
2. Open the address shown on the reader in a browser.
3. Open **Fonts**.
4. Upload the `.cpfont` files.

### Copy files to the SD card

1. Download a font family from the [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts).
2. Copy its folder to one of these locations on the SD card:

   - `/.fonts/` is hidden in most file managers.
   - `/fonts/` is visible in most file managers.

   CrossPoint reads both locations each time it starts. If the same family exists in both, it uses the copy in `/.fonts/`.

       SD Card Root/
       ├── .fonts/                     ← Hidden root (preferred)
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← Visible root (equally valid)
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. Insert the SD card and start the reader.

## Read Chinese, Japanese, or Korean

The built-in interface font does not contain Chinese, Japanese, or Korean characters. Without a suitable SD card font, book titles and file names can show empty boxes.

CrossPoint uses your selected SD card font for an interface line that contains these characters.

For Chinese, Japanese, or Korean in the interface, the font family needs `.cpfont` files at 8, 10, and 12 points. Use these sizes in addition to your preferred reading sizes.

**Settings → Reader → Font Size** lists every size in the family. If you do not want to see 8, 10, and 12 as reading choices, make one family for the interface and another for reading.

When converting your own font, include the UI sizes:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyCJKFont-Regular.otf \
      --intervals cjk \
      --sizes 8,10,12,14,16,18 \
      --style regular \
      --name MyCJKFont \
      --output-dir ./MyCJKFont/

Remember these points:

- Select a suitable SD card font in **Settings → Reader → Font Family**.
- A line that contains Chinese, Japanese, or Korean uses the SD card font for the entire line. For example, `三体 Vol.1` uses it for both parts.
- If you select a built-in reading font, CrossPoint cannot use the SD card font in the interface. The empty boxes return.

## Find ready-made fonts

The current list of pre-built fonts is maintained in the
[crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts).

## Advanced: convert your own font

This section is for people who use a computer command line. Most readers can download or upload a ready-made font instead.

To convert your own TrueType/OpenType fonts:

### Install the required tools

    pip install freetype-py fonttools

### Convert one style

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Convert several styles

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### Choose a character range

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020–U+007E (Basic Latin) |
| `latin1` | U+0080–U+00FF (Latin-1 Supplement) |
| `latin-ext` | European languages (Latin + Extended-A/B + punctuation + ligatures) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `hebrew` | Hebrew + Alphabetic Presentation Forms |
| `arabic` | Arabic + Supplement + Extended-A + Presentation Forms A/B (RTL, contextual shaping) |
| `georgian` | Georgian + Georgian Supplement |
| `armenian` | Armenian |
| `ethiopic` | Ethiopic + Extended |
| `vietnamese` | Vietnamese subset (ơ/ư and combining marks) |
| `ipa-chars` | IPA Extensions + Spacing Modifier Letters (phonetic transcription) |
| `punctuation` | General punctuation (U+2000–U+206F) |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables + Jamo + Compatibility Jamo |
| `cherokee` | Cherokee (historic + supplement block) |
| `tifinagh` | Tifinagh |
| `symbols` | Math, currency, arrows, box-drawing, misc symbols, dingbats |
| `reading` | Literary fiction coverage: Latin, Greek, Cyrillic, math/symbol blocks, supplemental punctuation, and CJK quote marks |
| `builtin` | Matches the firmware's built-in font conversion intervals |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

You can also specify arbitrary Unicode ranges directly:
`--intervals latin-ext,(0x2100-0x214F)`

To list all presets with codepoint counts:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### Other option

`--force-autohint` uses FreeType's automatic hinting instead of the font's own hinting. Use it if small text looks poor.

Install custom fonts via the web interface or manual SD card copy.

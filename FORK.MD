# FORK.md — Shortbread Changes

Changes made on top of [biscuit](https://github.com/yattsu/biscuit).

---

## 1. Home Screen — Collapsed to 5 Tiles

**Files:** `src/activities/apps/AppsMenuActivity.h`, `src/activities/apps/AppsMenuActivity.cpp`

Merged RECON, OFFENSE, DEFENSE, and COMMS into a single **NETWORK** tile.

**Before:** 8 tiles — RECON, OFFENSE, DEFENSE, COMMS, TOOLS, GAMES, READER, SETTINGS

**After:** 5 tiles — NETWORK, TOOLS, GAMES, READER, SETTINGS

Grid layout kept at 2 columns × 4 rows (original tile size). Bottom 3 slots are empty.

Removed includes and switch-case code for all dropped categories. Removed badge logic for RECON (tracker alert count) and DEFENSE (PIN not set warning).

---

## 2. NETWORK Tile — WiFi + Bluetooth Sub-Menu

**Files:** `src/activities/apps/AppsMenuActivity.cpp`

NETWORK tile opens an `AppCategoryActivity` with two entries:

| Entry | Activity | What it does |
|-------|----------|--------------|
| WiFi | `WifiConnectActivity` | Scan networks, show saved, enter password, connect |
| Bluetooth | `BleScannerActivity` | Scan nearby BLE devices |

`WifiConnectActivity` already handles: scan → list (saved networks marked) → select → password entry → connect.

---

## 3. Radar Home — Pentagon Layout (5 Nodes)

**Files:** `src/components/themes/radar/RadarHomeRenderer.h`, `src/components/themes/radar/RadarHomeRenderer.cpp`

Changed `NODE_COUNT` from 8 to 5. Updated node position vectors from octagon to pentagon (72° spacing, clockwise from top).

```cpp
// Pentagon unit vectors (sin/cos * 1000, clockwise from top)
static const int NODE_DX[5] = {    0,  951,  588, -588, -951 };
static const int NODE_DY[5] = { -1000, -309,  809,  809, -309 };
```

Updated `kRadarNodes[5]`:
```cpp
{"NETWORK",  2}, {"TOOLS", 32}, {"GAMES", 11}, {"READER", 5}, {"SETTINGS", 7}
```

---

## 4. Category Sub-Menus — Tile Grid Layout

**Files:** `src/activities/apps/AppCategoryActivity.h`, `src/activities/apps/AppCategoryActivity.cpp`

Replaced the scrolling list view in all category sub-menus with a **2-column tile grid**.

- Section headers are skipped in tile layout (tiles are self-descriptive)
- Scroll support: tracks `scrollRow`, keeps selected tile in view
- Navigation: Left/Right move between tiles linearly, Up/Down jump a full row
- Each tile shows: app name (bold, uppercase, `UI_10_FONT_ID`) + description (`SMALL_FONT_ID`, word-wrapped)
- Active-state dot indicator in top-right corner of tile

**Constants:** `TILE_COLS = 2`, `TILE_ROWS_VISIBLE = 4`, `TILE_GAP = 6px`

Tile height is computed dynamically: `(gridHeight - TILE_GAP * 3) / 4` — always fills screen with exactly 8 tiles (4 rows × 2 cols).

`ButtonNavigator` dependency removed. Replaced with direct `MappedInputManager` button checks.

---

## 5. Category Sub-Menus — Typography & Text Wrapping

**Files:** `src/activities/apps/AppCategoryActivity.cpp`

- Tile name: `UI_10_FONT_ID`, bold, uppercased at render time (one size below home screen `UI_12_FONT_ID`)
- Description: `SMALL_FONT_ID`, word-wrapped to fit tile width — no truncation
- Custom `drawWrapped()` helper splits on spaces and measures each line with `getTextWidth()` before drawing

---

## 6. Category Sub-Menus — Page Scroll on Long Press

**Files:** `src/activities/apps/AppCategoryActivity.cpp`

Side buttons (Up/Down) have two behaviours:

| Press | Action |
|-------|--------|
| Short press | Move one row (2 tiles) |
| Long press (≥ 600ms) | Jump one full page (4 rows = 8 tiles) |

`scrollRow` is kept in sync automatically — always follows `selectorIndex` into view.

---

## 7b. Removed Activities (no longer reachable from UI)

The following activities are still compiled but removed from all menus:

**RECON:** PacketMonitor, ProbeSniffer, Wardriving, CrowdDensity, DeviceFingerprinter, VendorOui, ApHistoryLogger, NetworkChange, PerimeterWatch, BleProximity, WifiHeatMap, SignalTriangulation, DeauthDetector

**OFFENSE:** OffenseMenu, Scan, BeaconTest, ApCloner, CaptivePortal, BleBeacon, BleSpam, BleKeyboard, AirTagTest, UsbHid, UsbMassStorage, BeaconFlood, SsidClone, Hunt, KeyCopier (offense), Loot (offense)

**DEFENSE:** Ghost, Emergency, TrackerDetector, Sweep, NetworkMonitor, PhoneTether, QuickWipe, ScreenDecoy, SecurityPin, SdEncryption

**COMMS:** MeshChat, SsidChannel, BleContactExchange, DeadDrop, BulletinBoard

> These source files remain on disk and compile cleanly. They can be re-wired to a menu at any time.

---

## 7. Dead Code Removal — Unused Activities Deleted

**Files deleted from** `src/activities/apps/`:

**RECON/OFFENSE/DEFENSE/COMMS (43 files, 21 pairs):**
AirTagTest, ApCloner, ApHistoryLogger, BeaconTest, BleBeacon, BleContactExchange, BleKeyboard, BleProximity, BleSpam, BreadcrumbTrail, BulletinBoard, CaptivePortal, CrowdDensity, DeadDrop, DeauthDetector, DeviceFingerprinter, Emergency, Ghost, Hunt, Loot, MeshChat, NetworkChange, NetworkMonitor, OffenseMenu, PacketMonitor, PerimeterWatch, PhoneTether, ProbeSniffer, QuickWipe, Scan, ScreenDecoy, SdEncryption, SecurityPin, SignalTriangulation, SsidChannel, Sweep, TrackerDetector, UsbHid, UsbMassStorage, VendorOui, Wardriving, WifiHeatMap, KeyCopier

**TOOLS trimmed (23 files, removed from menu + deleted):**
Totp, QrTotp, MedicalCard, Steganography, HostScanner, Ping, DnsLookup, HttpClient, MdnsBrowser, QrGenerator, MorseCode, Cipher, OtpGenerator, EventLogger, Flashcard, HabitTracker, Automation, VehicleFinder, TransitAlert, Barcode, WifiCreds, Countdown, EtchASketch

**TOOLS kept:** File Browser, Calculator, Clock, Unit Converter, Password Manager, WiFi Scanner

---

## 8. In-Book Menu — Cleaned Up, Reader Options Integration

**Files:** `src/activities/reader/ReaderOptionsActivity.h`, `src/activities/reader/ReaderOptionsActivity.cpp`, `src/activities/reader/EpubReaderMenuActivity.h`, `src/activities/reader/EpubReaderMenuActivity.cpp`, `src/activities/reader/EpubReaderActivity.cpp`

Ported `ReaderOptionsActivity` from [CrossInk](https://github.com/uxjulia/CrossInk). Added `READER_OPTIONS` as first item in `EpubReaderMenuActivity::buildMenuItems`. Handler in `EpubReaderActivity::onReaderMenuConfirm` launches `ReaderOptionsActivity` via `startActivityForResult`; on exit, resets `section` and calls `requestUpdate()` so font/layout changes take effect immediately.

`ReaderOptionsActivity` filters all `STR_CAT_READER` settings from `getSettingsList()`, renders them as a togglable list, and calls `SETTINGS.saveToFile()` on back.

Refactored `EpubReaderMenuActivity::render` to use `GUI.drawHeader()` and `GUI.drawList()` — same pattern as `ReaderOptionsActivity`. Progress summary shown as header subtitle. Orientation and page-turn values rendered via `rowValue` callback. Landscape gutter preserved.

---

## 9. EPUB Rendering — Ported from CrossInk

**Files:** `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`, `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`, `lib/Epub/Epub/blocks/TextBlock.cpp`, `lib/EpdFont/EpdFontFamily.h`, `lib/Epub/Epub/css/CssStyle.h`, `lib/Epub/Epub/css/CssParser.cpp`

Ported from [CrossInk](https://github.com/uxjulia/CrossInk):

| Feature | How |
|---------|-----|
| **Strikethrough** (`<s>`, `<strike>`, `<del>`) | `STRIKETHROUGH = 8` added to `EpdFontFamily::Style`. Parser tracks `strikethroughUntilDepth` + `effectiveStrikethrough`. `TextBlock::render` draws a mid-ascender horizontal line. |
| **Thicker underlines** | `TextBlock::render` draws two lines (underlineY and underlineY+1) instead of one. |
| **`<hr>` section breaks** | Parser emits a centered em-dash paragraph (`—————`) with 8px top/bottom margin. |
| **Table label cleanup** | Removed "Tab Row X, Cell Y:" italic prefix from table cells. Cells now render content directly. |
| **CSS `line-through`** | `CssTextDecoration::LineThrough = 2` added; `CssParser::interpretDecoration` matches `line-through`. |

---

## 10. Reader Fonts — ChareInk, Lexend Deca, Bitter

**Files:** `lib/EpdFont/builtinFonts/all.h`, `lib/EpdFont/builtinFonts/charein_*.h`, `lib/EpdFont/builtinFonts/lexenddeca_*.h`, `lib/EpdFont/builtinFonts/bitter_*.h`, `src/fontIds.h`, `src/main.cpp`, `src/CrossPointSettings.h`, `src/CrossPointSettings.cpp`, `src/SettingsList.h`, `lib/I18n/I18nKeys.h`, `lib/I18n/translations/*.yaml`

Replaced Bookerly / Noto Sans / Open Dyslexic with the CrossInk font set (noemoji variant). Font headers fetched via `scripts/download_crossink_fonts.py`.

| Old | New | Sizes |
|-----|-----|-------|
| Bookerly | **ChareInk** (serif, ink-optimised) | 8, 10, 12, 14, 16, 18, 20pt |
| Noto Sans | **Lexend Deca** (sans-serif, readability) | 8, 10, 12, 14, 16, 18, 20pt |
| Open Dyslexic | ~~Bitter~~ (dropped — flash budget; files remain on disk) | — |

`FONT_FAMILY` enum: `CHAREINK=0`, `LEXENDDECA=1`. `BITTER=2` no longer valid — saved settings with value 2 fall back to CHAREINK.

`FONT_SIZE` enum: `SMALL=0 (12pt)`, `MEDIUM=1 (14pt)`, `LARGE=2 (16pt)`, `EXTRA_LARGE=3 (18pt)`, `HUGE=4 (20pt)`, `TEENSY=5 (8pt)`, `TINY=6 (10pt)`. Flash: **87.2%** (Bitter removal freed ~1.1MB, enabling all 7 sizes for both fonts).

`scripts/download_crossink_fonts.py` — downloads all 84 font headers (sizes 8–20pt) from CrossInk's repo. Bitter headers remain on disk but are not included in the build.
- [ ] **Unicode emoji / symbols** — Requires Noto Emoji fallback font data. Flash budget concern: CrossInk uses separate build variants (tiny/xlarge/no_emoji) to manage size.
- [ ] **Redaction style** — Black-rect background on words with CSS `background-color: black`. Needs `wordBackgroundBlack` vector in `TextBlock`, CSS background parsing in `CssStyle`/`CssParser`, and `fillRect` in `TextBlock::render`.
- [ ] **Improved table rendering** — CrossInk uses a `BufferedTable` struct (max 8 cols, 64 cells) for proper column-width layout. Current code renders cell content left-aligned with no column tracking.

---

## 11. Home Screen — Recent Books with Cover Art

**Files:** `src/activities/apps/AppsMenuActivity.h`, `src/activities/apps/AppsMenuActivity.cpp`, `src/CrossPointSettings.h`, `src/SettingsList.h`, `lib/I18n/translations/english.yaml`

### What's implemented

The bottom recents section shows **one large book card** filling the full area. The most-recently-read book is displayed by default; pressing Down/Up cycles through up to 3 recent books (each fills the section). The card shows:

- **Cover art** as background — faded per the `Cover Opacity` setting
- **Title** (bold, `UI_10_FONT_ID`, truncated) pinned to bottom of card
- **Author** (`SMALL_FONT_ID`) below title
- **Reading progress** (`Ch.N p.X/Y`, right-aligned) — read from `progress.bin` without loading the EPUB
- **Book index indicator** (`1 / 3` etc.) top-right corner
- **Selection state**: inverted fill when recents zone is active

### Navigation

| Input | Action |
|-------|--------|
| Down (from tiles) | Enter recents zone, show book 0 |
| Down (in recents) | Cycle to next book; if at last → back to tiles |
| Up (in recents) | Cycle to prev book; if at first → back to tiles |
| Confirm (in recents) | Open selected book in reader |

### Cover loading (async, two-pass)

1. `onEnter()` → `loadRecentBooks()` (reads `progress.bin` per book, no EPUB load)
2. First `render()` → draws placeholder border, sets `firstRenderDone = true`, calls `requestUpdate()`
3. Second `render()` → `loadCovers()` runs: generates BMP thumbs via `epub.generateThumbBmp(coverThumbH)` / `xtc.generateThumbBmp(coverThumbH)` for any missing thumbs; calls `requestUpdate()`
4. Third `render()` → loads BMP from SD, draws faded cover

**Thumb height** — computed in `render()` as `((pageWidth - sidePad*2) * 3) / 2 + 50`. This ensures the portrait cover's width exceeds tile width so `drawBitmap` downscales to fill width (the renderer only downscales; upscaling is not supported).

**Fill + top-crop** — `cropY` is computed so the image fills tile width and height exactly:
```cpp
float widthScale = (float)(w - 2) / bmpW;
float scaledH = bmpH * widthScale;
if (scaledH > booksH - 2) cropY = 1.0f - ((float)(booksH - 2) / scaledH);
renderer.drawBitmap(bitmap, x+1, booksTop+1, w-2, booksH-2, 0.0f, cropY);
```

### Cover Opacity setting

Added to **Settings → Display** as `Cover Opacity` (key `coverOpacity`):

| Value | Label | Effect |
|-------|-------|--------|
| 0 | Off | No cover drawn |
| 1 | Light (25%) | Keep pixels at `x%2==0 && y%2==0` |
| 2 | Medium (50%) | Keep pixels at `(x+y)%2==0` |
| 3 | Full | No fade overlay |

Default: **Light (25%)**. Maps exactly to the renderer's native dither densities.

---

## 12. Boot Animation — Removed

**Files:** `src/activities/boot_sleep/BootActivity.cpp`

Stripped all rendering from `BootActivity::onEnter()`. Logo, app name, and "Booting…" text no longer drawn on startup. Device boots straight to the home screen with no splash delay.

Removed includes: `GfxRenderer.h`, `I18n.h`, `fontIds.h`, `images/Logo120.h`.

---

## 13. Sleep Wake — Nav Buttons (USB only)

**Files:** `lib/hal/HalGPIO.cpp`, `lib/hal/HalPowerManager.cpp`

GPIO1 and GPIO2 (nav ADC pins) added to the deep-sleep GPIO wakeup mask alongside the power button (GPIO3). Pressing a nav button while on USB power wakes the device and boots normally.

**Which buttons wake:** Left, Right, Down — their resistor-divider voltage drops below the 1.65 V logic LOW threshold. Up, Confirm, and Back stay above it and cannot trigger wakeup.

**Battery limitation:** On battery the MOSFET latch (GPIO13) cuts all power including RTC. The chip is completely off; only the hardware power button (which physically restores power) can wake it. This is a hardware constraint, not software.

`getWakeupReason()` updated to use `esp_sleep_get_gpio_wakeup_status()` to distinguish power-button vs nav-button wakeup on USB. Nav-button wakeup returns `Other` → boots normally without triggering `verifyPowerButtonWakeup`.

---

## ✅ Build Status

All changes complete. Build passes clean.

---

## TODO

### Reader
- [ ] **Dictionary on press-and-hold** — Long press on a word in the reader triggers a dictionary lookup overlay, similar to macOS. Requires: word boundary detection at tap/button position, dictionary data source on SD card, popup overlay renderer.
- [ ] **Word selector** — Press-and-hold also activates a word highlight selector (extend selection left/right with buttons), like macOS text selection. Feeds into the dictionary lookup or copy buffer.



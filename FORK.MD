# FORK.md — Biscuit Fork Changes

Changes made on top of the upstream biscuit firmware.

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

## Removed Activities (no longer reachable from UI)

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

## ✅ Build Status

All changes complete. Build passes clean.

---

## TODO

### GitHub
- [ ] **GitHub fork badge** — Detach `deepakvettickal/crosspoint-lean` from the crosspoint-reader fork network via GitHub Support (`support.github.com`), then re-fork crosspoint-reader into `shortbread` to get the official fork badge. Badge is cosmetic only — `shortbread` already tracks `crosspoint-reader` as `upstream` remote.

### UI / Navigation
- [ ] **Remove boxes around navigation indicators** — Bottom button hints currently render each label inside a drawn rectangle. Remove the box outlines, show labels as plain text only.
- [ ] **Smaller BACK / SELECT with icons** — Replace text labels with compact icons. Candidate: `<<` for BACK, `>>` for SELECT. Affects `drawButtonHints` in `BaseTheme.cpp` (and `NoirTheme.cpp`, `MilitaryTheme.cpp` overrides). Apply consistently across all themes.
- [ ] **Move navigation indicators closer to border** — Reduce bottom padding so hints sit closer to the screen edge. Adjust `buttonHintsHeight` metric and hint Y position in `BaseTheme.cpp`.

### Reader
- [ ] **Dictionary on press-and-hold** — Long press on a word in the reader triggers a dictionary lookup overlay, similar to macOS. Requires: word boundary detection at tap/button position, dictionary data source on SD card, popup overlay renderer.
- [ ] **Word selector** — Press-and-hold also activates a word highlight selector (extend selection left/right with buttons), like macOS text selection. Feeds into the dictionary lookup or copy buffer.

### Settings
- [ ] **Dark theme inside SETTINGS** — Settings screens render with inverted colours (dark background, light text) regardless of the global UI theme. Scoped to `SettingsActivity` and its child screens only.

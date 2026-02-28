# What's New in CrossPoint (Laird Fork)

This document covers both features **added in this fork** and features from the **base CrossPoint firmware** — clearly labelled so you know what's new vs what's already there.

> 🔵 **This fork** — added in `laird/crosspoint-reader` (`feature/rss-feed-sync` branch)  
> ⚪ **Base CrossPoint** — present in upstream `crosspoint-reader/crosspoint-reader`

---

## 🔵 PULSR Theme

The UI has been redesigned with the **PULSR** theme — a high-contrast black-and-white aesthetic optimized for e-ink readability. All screens use the PULSR theme by default.

Key design elements:
- Bold title bars (white text on black)
- Selected rows highlighted with a dithered halftone pattern
- PULSR font (Antonio) used throughout the UI chrome
- Clean sans-serif body text for settings and menus

![Home Screen](images/screenshots/home.png)

*Home screen showing the PULSR theme with book covers, navigation buttons, and the persistent left sidebar.*

---

## 🔵 Persistent Left Sidebar

Every screen features a consistent **left sidebar** providing always-accessible controls:

| Element | Function |
|---------|----------|
| **PWR** | Power button / sleep indicator |
| **HTTP** | Active indicator when web server / file transfer is running |
| **FEED** | Animated indicator during RSS feed sync (see below) |
| **↑ / ↓** | Page up / page down navigation |
| **Battery bar** | Vertical fill gauge with percentage |

The sidebar layout adapts per screen — for example, showing **SYNC** in the OPDS browser, and showing the **FEED** pill animating through states during a feed sync.

---

## 🔵 RSS Feed Sync

The reader automatically syncs content from an RSS feed server over WiFi. Every time the reader connects to WiFi, it fetches new items from the configured feed URL and downloads them to the SD card.

### How it works

1. Reader connects to WiFi
2. Fetches `feed.xml` from the configured feed server
3. For each new item (newer than last sync timestamp), downloads the file
4. Places files in the correct directory on the SD card (`/Books/chip/`, `/sleep/`, `/trips/`, etc.)
5. Updates the last-sync timestamp so items are never re-downloaded

### Feed Sync indicator states

The **FEED pill** in the left sidebar shows live sync progress:

| State | Meaning |
|-------|---------|
| `FEED` | Fetching feed XML |
| `SYNC` | Parsing items |
| `3/12` | Downloading (item count) |
| `DONE` | Sync complete (clears after 5s) |
| `ERR` | Error occurred (check feed-sync.log) |

### Configuration

Settings → System → **Feed Sync**:

- **Feed URL** — URL of the RSS feed server (e.g. `http://192.168.0.83:8090/feed.xml`)
- **News Days** — How many days of news items to retain (default: 7)
- **Allow Firmware Updates** — Whether to apply OTA firmware from the feed (disabled by default; ⚠️ trusted feeds only)

![System Settings](images/screenshots/settings_syst.png)

*System settings tab showing Feed Sync, WiFi Networks, OPDS Browser, and other system options.*

### Feed server

A companion **feed server** (`feed-server/feed_server.py`) is included in the repo. It:
- Serves EPUBs, BMPs, news JSON, and firmware as RSS
- Requires no external Python dependencies (stdlib only)
- Sorts all items newest-first (required by reader dedup)
- Can use symlinks — point subdirs at your existing content library

See [`feed-server/README.md`](../feed-server/README.md) for setup instructions.

---

## 🔵 OTA Firmware Updates via Feed

> ⚠️ **Security warning:** Only enable this if you control the feed server and trust it completely. A malicious feed server could deliver compromised firmware that fully owns your device. This feature is intended for developers building and delivering their own personalized firmware to their own Xteink reader — for example, if you've forked this repo and want to push your own builds over WiFi. **Do not enable it on a feed server you did not set up yourself.** The setting defaults to OFF.

When **Allow Firmware Updates** is enabled in Settings → System → Feed Sync, the reader will download and apply new firmware automatically from the configured feed. The feed server places the latest `.bin` in `content/firmware/` and the reader flashes it on next boot.

The firmware update progress is shown on-screen:

> *"Updating firmware... [progress bar] XX%  
> Do not power off"*

After flashing, the reader reboots automatically.

---

## 🔵 Settings: Improved Column Navigation

Settings have always been organized into four columns (DISP, READ, CTRL, SYST). The improvement is that the **left/right hardware buttons now switch between columns**, making it much faster to jump between setting categories without hunting through menus.

### DISP — Display

![Display Settings](images/screenshots/settings_disp.png)

| Setting | Description |
|---------|-------------|
| **Sleep Screen** | Choose sleep screen style (Custom, Cover, etc.) |
| **Sleep Screen Cover Mode** | How cover art is fitted (Fit, Fill, Stretch) |
| **Sleep Screen Cover Filter** | Optional image filter for cover |
| **Status Bar** | Status bar style (Full+%, minimal, off) |
| **Hide Battery %** | When to hide battery percentage |
| **Refresh Frequency** | Full e-ink refresh every N pages |
| **UI Theme** | Select UI theme (PULSR, etc.) |
| **Sunlight Fading Fix** | Compensates for e-ink fading in bright light |

### READ — Reading

![Reading Settings](images/screenshots/settings_read.png)

| Setting | Description |
|---------|-------------|
| **Reader Font Family** | Bookerly, Noto Sans, OpenDyslexic, etc. |
| **UI Font Size** | Small / Medium / Large |
| **Reader Line Spacing** | Compact / Normal / Relaxed |
| **Reader Screen Margin** | Pixel margin around text |
| **Reader Paragraph Alignment** | Left / Justify |
| **Embedded Style** | Respect EPUB embedded CSS |
| **Hyphenation** | Enable auto-hyphenation |
| **Reading Orientation** | Portrait / Landscape |
| **Extra Paragraph Spacing** | Additional space between paragraphs |
| **Text Anti-Aliasing** | Smoother font rendering |

### CTRL — Controls

![Controls Settings](images/screenshots/settings_ctrl.png)

| Setting | Description |
|---------|-------------|
| **Remap Front Buttons** | Customize front button actions (→ submenu) |
| **Side Button Layout** | Prev/Next or Up/Down page turns |
| **Long-press Chapter Skip** | Long-press side buttons to jump chapters |
| **Short Power Button Click** | Action on short power press (Sleep / Ignore) |

### SYST — System

| Setting | Description |
|---------|-------------|
| **Time to Sleep** | Auto-sleep timeout (minutes) |
| **WiFi Networks** | Manage saved WiFi credentials |
| **Feed Sync** | Configure RSS feed URL and options |
| **KOReader Sync** | KOReader reading position sync |
| **OPDS Browser** | Browse OPDS catalogs |
| **Clear Reading Cache** | Free SD card space from cached data |
| **Check for Updates** | Manual OTA firmware check |
| **Language** | UI language (English, Polish, Dutch, etc.) |

---

## ⚪ File Transfer Mode

The reader can receive files over WiFi in three modes:

![File Transfer](images/screenshots/file_transfer.png)

| Mode | Description |
|------|-------------|
| **Join a Network** | Connect to home WiFi, then upload via browser at `http://[reader-ip]/` |
| **Calibre Wireless** | Use Calibre's wireless device plugin |
| **Create Hotspot** | Reader creates its own WiFi network for direct connection |

The file transfer screen now shows **live file arrival updates** — files appear in the list as they finish downloading (both from HTTP upload and RSS feed sync).

---

## ⚪ OPDS Browser

Browse and download books directly from OPDS catalog servers (Calibre, Jellyfin, public libraries, etc.).

![OPDS Browser](images/screenshots/opds.png)

Configure the server URL in Settings → System → OPDS Browser.

---

## ⚪ WiFi Network Management

![WiFi Scan](images/screenshots/wifi_scan.png)

The WiFi scan screen shows available networks with a **Back button** to cancel. Selecting a network opens the on-screen keyboard for password entry.

---

## 🔵 On-Screen Keyboard (sidebar fix)

![Keyboard](images/screenshots/keyboard.png)

Full QWERTY keyboard for password and URL entry. Keys are no longer clipped by the sidebar — the full `q`, `a`, `z`, and Shift keys are visible.

---

## 🔵 Screenshot Tour

Hold **Power + Confirm** for 1.5 seconds to trigger a screenshot tour. The device automatically navigates through all major screens and saves `.bmp` captures to `/screencap/` on the SD card.

Screenshots can be viewed in the file browser or downloaded via the web transfer interface.

---

## 🔵 Log Files

Feed sync activity is logged to `/.crosspoint/feed-sync.log` on the SD card. `.log` files are readable in the on-device text viewer and downloadable via the browser interface.

---

## ⚪ Multilingual Support

Settings → System → Language. Supported languages include:
- English
- Polish
- Dutch
- (more via community contributions)

---

## 🔵 Feed Server Quick Start

```bash
# Clone the repo
git clone https://github.com/laird/crosspoint-reader.git
cd crosspoint-reader/feed-server

# Point content dirs at your library (symlinks work)
ln -s ~/Documents/epub/Books/chip content/books/chip
ln -s ~/Documents/epub/Books/erotic content/books/erotic
ln -s ~/Documents/epub/trips content/trips
ln -s ~/clawd/data/sleep-bmps content/sleep

# Start the server
python3 feed_server.py --feed-url http://YOUR_SERVER_IP:8090

# On the reader: Settings → System → Feed Sync → Feed URL
# Set to: http://YOUR_SERVER_IP:8090/feed.xml
```

New files dropped into any content directory automatically appear in the feed on the next request — no restart needed.

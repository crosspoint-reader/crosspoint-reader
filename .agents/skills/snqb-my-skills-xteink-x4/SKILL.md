---
name: xteink-x4
description: Xteink X4 e-ink pocket reader — hardware specs, firmware (stock/Crosspoint/Papyrix), book management, ESP32-C3 development, flashing, community tools, and troubleshooting. Use when working with the Xteink X4 device, its firmware, or its ecosystem.
---

# Xteink X4 — Pocket E-Ink Reader

## Device Overview

The **Xteink X4** is a tiny, ultra-portable 4.3" e-ink reader (~$69). No touchscreen — button-only navigation. ESP32-C3 based, fully hackable. MagSafe-compatible. Community firmware available.

**Official site**: https://www.xteink.com/

## Hardware Specs

| Spec | Value |
|---|---|
| Display | 4.26" E-Ink (800×480), 220 PPI, no backlight |
| Processor | ESP32-C3 (RISC-V RV32IMC, 160 MHz) |
| RAM | ~380 KB usable SRAM |
| Flash | 16 MB |
| Storage | 32 GB microSD (FAT32), expandable |
| Battery | 650 mAh LiPo, ~14 days (1-3h/day) |
| Connectivity | WiFi 802.11 b/g/n (2.4 GHz), BLE 5.0 |
| Ports | USB-C (charge + flash) |
| Dimensions | 114 × 69 × 5.9 mm, 74g |
| Built-in | MagSafe magnet |
| Buttons (bottom) | Back, Confirm, Left, Right |
| Buttons (side) | Power, Volume Up, Volume Down, Reset |
| Supported formats | EPUB, TXT, BMP, JPG (stock); EPUB (Crosspoint) |

### Power Consumption
- Active reading: ~50 mA
- WiFi active: ~150 mA
- Deep sleep: ~10 µA

### Display Panel
- Model: GDEQ0426T82
- Type: Electrophoretic (E-Ink)

---

## Firmware Options

### 1. Stock Firmware (Closed Source)
- Ships with the device
- Supports EPUB, TXT, BMP, JPG
- Can display boarding passes, membership cards, calendars
- Flash/revert via: https://xteink.dve.al/

### 2. CrossPoint Reader (Recommended Community Firmware)
- **Repo**: https://github.com/crosspoint-reader/crosspoint-reader (2100+ ⭐, MIT)
- Fully open-source drop-in replacement
- Built with **PlatformIO**, targets ESP32-C3
- EPUB 2/3 parsing and rendering
- WiFi book upload + OTA updates
- Configurable fonts, layout, display, status bar
- Screen rotation, custom sleep screens
- Calibre integration via plugin
- Multi-language support (EN, ES, FR, DE, IT, PT, RU, UK, PL, SV, NO, etc.)

#### Install CrossPoint (Web — Easiest)
```
1. Connect X4 via USB-C, wake/unlock device
2. Go to https://xteink.dve.al/
3. Click "Flash CrossPoint firmware"
```

#### Install CrossPoint (Manual / Development)
```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader
pio run --target upload
```

#### Revert to Stock
```
1. Go to https://xteink.dve.al/
2. Flash latest official firmware, OR
3. Use "Swap boot partition" at https://xteink.dve.al/debug
```

#### CrossPoint Button Controls

**Reading Mode:**
| Action | Button |
|---|---|
| Next page | Right / Volume Down |
| Previous page | Left / Volume Up |
| Next chapter | Long-press Right (configurable) |
| Previous chapter | Long-press Left (configurable) |
| Back to home | Back |

**Navigation:**
| Action | Button |
|---|---|
| Move cursor | Left / Right |
| Select | Confirm |
| Go back | Back |
| Scroll page | Long-press Left/Right |

#### CrossPoint Settings
- Sleep screen: Dark/Light/Custom/Cover/None
- Status bar: None/No Progress/Percentage/Book Bar/Chapter Bar
- Text anti-aliasing: On/Off (slows page turns slightly)
- Extra paragraph spacing: On/Off
- Reading orientation: Portrait/Landscape CW/Inverted/Landscape CCW
- Font family: Bookerly (default), Noto Sans
- Button layout: Configurable (4 presets)
- Power button behavior: Ignore/Sleep/Page Turn

### 3. Papyrix Reader (Alternative Community Firmware)
- **Repo**: https://github.com/bigbag/papyrix-reader (139 ⭐, MIT)
- Lightweight, EPUB/FB2/MD/TXT support
- WiFi transfers, custom themes & fonts

### 4. Language Forks
- **Thai**: https://github.com/gozilla-paradise/crosspoint-reader-th
- **Chinese optimized**: https://github.com/icannotttt/crosspoint-chinesetype

---

## Book Management

### Loading Books via WiFi (CrossPoint)
1. On device: Navigate to **File Upload** screen
2. Connect to WiFi network
3. Device hosts a web server — open the displayed IP in browser
4. Drag & drop EPUB files

### Loading Books via Calibre
1. Install CrossPoint Calibre plugin from https://github.com/crosspoint-reader/calibre-plugins/releases
2. On device: File Transfer → Connect to Calibre → Join network
3. Computer must be on same WiFi
4. In Calibre: click "Send to device"

### Loading Books via SD Card
1. Remove microSD from device
2. Copy DRM-free EPUB/TXT files to card (FAT32)
3. Re-insert card

### Loading Books via curl (Advanced)
```bash
# Upload an EPUB to the device web server
curl -F "file=@book.epub" http://<device-ip>/upload
```

### Important Notes
- **DRM-free only** — Kindle DRM books won't work. Strip DRM first or use DRM-free sources
- **EPUB format preferred** — best rendering support
- CrossPoint caches chapter data to `.crosspoint/` on SD card
- Moving book files resets reading progress (cache keyed by path hash)

---

## Development

### Prerequisites
- **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
- Python 3.8+
- USB-C cable

### Build & Flash
```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader
pio run --target upload
```

### Serial Debugging
```bash
pip install pyserial colorama matplotlib
# macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
# Linux
python3 scripts/debugging_monitor.py
```

### Memory Constraints
- Only ~380 KB usable RAM — be extremely conservative
- CrossPoint aggressively caches to SD card to minimize RAM usage
- Chapter data cached on first load, served from cache after

### Cache Structure
```
.crosspoint/
├── epub_<hash>/
│   ├── progress.bin    # Reading position
│   ├── cover.bmp       # Cover image
│   ├── book.bin         # Metadata
│   └── sections/
│       ├── 0.bin        # Chapter data
│       └── ...
```

### Contributing to CrossPoint
1. Fork the repo
2. Create branch (`feature/my-feature`)
3. Check the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas)
4. Submit PR

---

## Community Ecosystem

### Firmware & Readers
| Project | Description | Stars |
|---|---|---|
| [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) | Main community firmware (EPUB) | 2158 |
| [papyrix-reader](https://github.com/bigbag/papyrix-reader) | Lightweight alt firmware (EPUB/FB2/MD/TXT) | 139 |
| [TrustyReader](https://github.com/HookedBehemoth/TrustyReader) | Firmware in Rust | 8 |
| [microslate-firmware](https://github.com/Josh-writes/microslate-firmware) | Writing firmware | 11 |

### Tools & Converters
| Project | Description | Stars |
|---|---|---|
| [xteink-flasher](https://github.com/crosspoint-reader/xteink-flasher) | Web-based flashing tool | 65 |
| [cr2xt](https://github.com/CrazyCoder/cr2xt) | Desktop converter to XTC binary book format | 86 |
| [cbz2xtc](https://github.com/tazua/cbz2xtc) | Manga CBZ → XTC converter | 34 |
| [xtcjs](https://github.com/varo6/xtcjs) | Browser-based CBZ → XTC converter | 50 |
| [XTEink-Web-Font-Maker](https://github.com/lakafior/XTEink-Web-Font-Maker) | Web font converter to XTEink binary format | 27 |

### Companion Apps & Extensions
| Project | Description | Stars |
|---|---|---|
| [send-to-x4](https://github.com/Xatpy/send-to-x4) | Browser extension to send articles as EPUB | 48 |
| [hojo](https://github.com/meta-boy/hojo) | Unofficial Android companion app | 52 |
| [X4Term](https://github.com/penk/X4Term) | Pocket VT100 terminal | 25 |
| [xteink-tamagotchi](https://github.com/maddiedreese/xteink-tamagotchi) | Tamagotchi game | 60 |
| [esphome-component-xteink](https://github.com/ngxson/esphome-component-xteink) | ESPHome integration | 11 |

### Reference
| Project | Description | Stars |
|---|---|---|
| [xteink-x4-sample](https://github.com/CidVonHighwind/xteink-x4-sample) | Sample code | 58 |
| [xteink-x4-english](https://github.com/Joseph-Cannaday/xteink_x4-english) | English resources | 56 |
| [Xteink-X4 (sunwoods)](https://github.com/sunwoods/Xteink-X4) | Hardware/software info collection | 43 |
| [Crosspoint-Emulator](https://github.com/jonmooreai/Crosspoint-Emulator) | Crosspoint firmware emulator | 21 |

---

## Troubleshooting

### Device Frozen / Bootloop
1. Press **Reset** button (right side)
2. Immediately press and hold **Power** for a few seconds
3. If still stuck, reflash via https://xteink.dve.al/debug → "Swap boot partition"

### Books Not Showing
- Ensure files are DRM-free EPUB or TXT
- Check SD card is FAT32 formatted
- Files must be at root or in folders on SD card

### Poor Text Rendering
- Try enabling **Text Anti-Aliasing** in CrossPoint settings (slight page turn slowdown)
- Switch font to **Noto Sans** for better Unicode coverage
- Some complex layouts may not render well on 4.3" screen

### WiFi Upload Issues
- Device and computer must be on same network
- Only 2.4 GHz WiFi supported (no 5 GHz)
- **VPN blocks local access**: If VPN routes all traffic through tunnel (utun), device is unreachable. Fix: `sudo route add -host <device-ip> -interface en0`
- HTTP POST dies on files >128KB — use WebSocket (port 81) or x4m for uploads

### Battery Draining Fast
- Disable WiFi when not transferring books
- Turn off Bluetooth if unused
- Reduce page turns (deep sleep between reads uses ~10 µA)

### Clearing Cache
```
Delete .crosspoint/ directory on SD card to reset all cached data.
Warning: This resets all reading progress.
```

---

---

## CrossPoint Web API

When the device is in File Upload mode (WiFi connected), it exposes:

- **HTTP Server**: Port 80
- **WebSocket Server**: Port 81 (fast binary upload)
- **mDNS**: `crosspoint.local` (if network supports it), otherwise use IP shown on device

### HTTP Endpoints

| Method | Endpoint | Description |
|---|---|---|
| `GET /` | Home page UI |
| `GET /files` | File browser UI |
| `GET /api/status` | Device status JSON |
| `GET /api/files?path=/` | List files/folders JSON |
| `POST /upload?path=/` | Upload file (multipart form) |
| `POST /mkdir` | Create folder (form: `name`, `path`) |
| `POST /delete` | Delete file/folder (form: `path`, `type`) |

### Device Status (`GET /api/status`)
```json
{
  "version": "1.0.0",
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600
}
```

### File Listing (`GET /api/files`)
```json
[
  {"name": "MyBook.epub", "size": 1234567, "isDirectory": false, "isEpub": true},
  {"name": "Notes", "size": 0, "isDirectory": true, "isEpub": false}
]
```
Hidden files (`.`-prefixed) and system folders (`System Volume Information`, `XTCache`) are filtered out.

### Upload (`POST /upload`)
```bash
# Upload to root
curl -X POST -F "file=@mybook.epub" http://192.168.0.104/upload

# Upload to subfolder
curl -X POST -F "file=@mybook.epub" "http://192.168.0.104/upload?path=/Books"
```
**⚠ HTTP upload dies on files >128KB** (ESP32 timeout). Use WebSocket (port 81) for anything larger.

### Create Folder (`POST /mkdir`)
```bash
curl -X POST -d "name=NewFolder&path=/" http://192.168.0.104/mkdir
```

### Delete (`POST /delete`)
```bash
curl -X POST -d "path=/Books/old.epub&type=file" http://192.168.0.104/delete
```
Protected items: `.`-prefixed, `System Volume Information`, `XTCache`.

### WebSocket Fast Upload (Port 81) — USE THIS FOR LARGE FILES
```
ws://192.168.0.104:81/

Protocol:
  Client → "START:<filename>:<size>:<path>"
  Server → "READY"
  Client → [binary chunks]
  Server → "PROGRESS:<received>:<total>"
  Server → "DONE" or "ERROR:<message>"
```

**Optimal settings** (benchmarked):
- Chunk size: **8KB** (16KB+ crashes ESP32)
- Pacing delay: **5ms** between chunks
- Throughput: **~315 KB/s** (ESP32 SPI→SD write ceiling)
- 91MB XTCH: ~5 min, 155MB: ~8.5 min

Speed is limited by the ESP32-C3's SPI bus to the SD card (~400 KB/s max), NOT WiFi or the card itself. Faster SD cards won't help.

**CLI tool**: `x4ws <file> [ip] [dest]` (installed at `~/bin/x4ws`)

### Network Modes
- **STA (Station)**: Connects to existing WiFi, IP from DHCP
- **AP (Access Point)**: Creates hotspot, default IP `192.168.4.1`

---

## XTC/XTG/XTH/XTCH Format Specification

Four binary formats for ESP32 e-paper displays. All multi-byte values are **Little-Endian**.

### Format Family

| Format | Extension | Purpose | Bit Depth |
|---|---|---|---|
| **XTG** | `.xtg` | Monochrome image | 1-bit/pixel |
| **XTH** | `.xth` | 4-level grayscale image | 2-bit/pixel |
| **XTC** | `.xtc` | Comic container (multiple XTG pages) | 1-bit |
| **XTCH** | `.xtch` | Comic container variant (XTH pages) | 2-bit |

### Magic Numbers

| Format | uint32_t (LE) | Bytes | ASCII |
|---|---|---|---|
| XTG | `0x00475458` | `58 54 47 00` | `XTG\0` |
| XTH | `0x00485458` | `58 54 48 00` | `XTH\0` |
| XTC | `0x00435458` | `58 54 43 00` | `XTC\0` |
| XTCH | `0x48435458` | `58 54 43 48` | `XTCH` |

### Display Constants
- Width: **480 px**
- Height: **800 px**

### XTG/XTH Page Header (22 bytes)

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| 0x00 | 4 | uint32 | magic | `XTG_MAGIC` or `XTH_MAGIC` |
| 0x04 | 2 | uint16 | width | Image width (pixels) |
| 0x06 | 2 | uint16 | height | Image height (pixels) |
| 0x08 | 1 | uint8 | colorMode | 0 = monochrome |
| 0x09 | 1 | uint8 | compression | 0 = uncompressed |
| 0x0A | 4 | uint32 | dataSize | Bitmap data size (bytes) |
| 0x0E | 8 | uint64 | md5 | MD5 checksum (first 8 bytes, optional) |

**XTG bitmap data** (after header):
- Row-major, top-to-bottom, left-to-right
- 8 pixels/byte, MSB = leftmost pixel
- Bit 0 = Black, Bit 1 = White
- `dataSize = ((width + 7) / 8) * height`

**XTH bitmap data** (after header):
- Two bit planes stored sequentially
- **Column-major (vertical scan)**: columns right-to-left, 8 vertical pixels/byte
- `pixelValue = (bit1 << 1) | bit2`
- `dataSize = ((width * height + 7) / 8) * 2`

**XTH grayscale LUT** (swapped middle values!):

| Value | Binary | Display |
|---|---|---|
| 0 | `00` | White |
| 1 | `01` | **Dark Grey** |
| 2 | `10` | **Light Grey** |
| 3 | `11` | Black |

### XTC Container Header (56 bytes)

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| 0x00 | 4 | uint32 | magic | `XTC_MAGIC` or `XTCH_MAGIC` |
| 0x04 | 1 | uint8 | versionMajor | 1 |
| 0x05 | 1 | uint8 | versionMinor | 0 |
| 0x06 | 2 | uint16 | pageCount | Total pages |
| 0x08 | 1 | uint8 | readDirection | 0=L→R, 1=R→L (manga), 2=Top→Bottom |
| 0x09 | 1 | uint8 | hasMetadata | 0-1 |
| 0x0A | 1 | uint8 | hasThumbnails | 0-1 |
| 0x0B | 1 | uint8 | hasChapters | 0-1 |
| 0x0C | 4 | uint32 | currentPage | Last read page (1-based) |
| 0x10 | 8 | uint64 | metadataOffset | |
| 0x18 | 8 | uint64 | pageTableOffset | |
| 0x20 | 8 | uint64 | dataOffset | |
| 0x28 | 8 | uint64 | thumbOffset | |
| 0x30 | 4 | uint32 | chapterOffset | |
| 0x34 | 4 | uint32 | padding | |

### Page Table Entry (16 bytes per page)

| Offset | Size | Type | Field |
|---|---|---|---|
| 0x00 | 8 | uint64 | dataOffset (absolute) |
| 0x08 | 4 | uint32 | dataSize |
| 0x0C | 2 | uint16 | width |
| 0x0E | 2 | uint16 | height |

### Metadata (256 bytes, optional)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 128 | title (UTF-8, null-terminated) |
| 0x80 | 64 | author |
| 0xC0 | 32 | publisher |
| 0xE0 | 16 | language (e.g. "en-US") |
| 0xF0 | 4 | createTime (Unix timestamp) |
| 0xF4 | 2 | coverPage (0-based, 0xFFFF=none) |
| 0xF6 | 2 | chapterCount |
| 0xF8 | 8 | reserved |

### Chapter Entry (96 bytes per chapter, optional)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 80 | chapterName (UTF-8, null-terminated) |
| 0x50 | 2 | startPage (0-based) |
| 0x52 | 2 | endPage (0-based, inclusive) |
| 0x54 | 12 | reserved |

### File Layout
```
[Header: 56 bytes]
[Metadata: 256 bytes]      (optional, at metadataOffset)
[Chapters: N × 96 bytes]   (optional, at chapterOffset)
[Page Index: N × 16 bytes] (at pageTableOffset)
[Page Data: XTG/XTH blobs] (at dataOffset)
[Thumbnails]                (optional, at thumbOffset)
```

### Conversion Tools

| Tool | Input | Output | Platform |
|---|---|---|---|
| [cr2xt](https://github.com/CrazyCoder/cr2xt) | EPUB/FB2/MOBI/DOC/HTML/etc. | XTC/XTCH | Desktop (Qt, Win/Mac/Linux) |
| [cbz2xtc](https://github.com/tazua/cbz2xtc) | CBZ (manga) | XTC | CLI (Python + Pillow) |
| [xtcjs](https://github.com/varo6/xtcjs) | CBZ | XTC | Browser-based |

### CrossPoint Cache Formats

CrossPoint uses its own binary cache (not XTC). Key files in `.crosspoint/epub_<hash>/`:

**`book.bin` (v3)**: Length-prefixed UTF-8 strings. Contains version, LUT offset, spine count, TOC count, metadata (title, author, cover href), spine entries (href, cumulative size, TOC index), TOC entries (title, href, anchor, level, spine index).

**`section.bin` (v8)**: Pre-rendered page layouts. Contains version, font ID, line compression, paragraph spacing, viewport dimensions, page count. Each page has positioned word elements with x/y coords, text, style (regular/bold/italic), and block alignment (justified/left/center/right).

---

## Key Links
- **Official site**: https://www.xteink.com/
- **Web flasher**: https://xteink.dve.al/
- **CrossPoint firmware**: https://github.com/crosspoint-reader/crosspoint-reader
- **CrossPoint user guide**: https://github.com/crosspoint-reader/crosspoint-reader/blob/master/USER_GUIDE.md
- **XTC format spec**: https://github.com/CrazyCoder/crengine-ng/blob/main/crengine/docs/XtcFormat.md
- **Reddit community**: r/xteink (active subreddit)
- **Calibre plugin**: https://github.com/crosspoint-reader/calibre-plugins

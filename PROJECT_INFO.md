# Project Information: CrossPoint Reader

CrossPoint Reader is an open-source, minimalist firmware specifically designed for the **Xteink X4** e-reader, which is powered by an **ESP32-C3** microcontroller. The project prioritizes efficiency, focus, and a small RAM footprint (~380KB available) while providing a high-quality EPUB reading experience.

## Build Instructions

### Prerequisites
- **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
- Python 3.8+
- Git (for submodules)

### Cloning and Setup
```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader

# If submodules were missed:
git submodule update --init --recursive
```

### Building Firmware
To build the default firmware environment:
```bash
pio run -e default
```

### Flashing
To build and flash via USB-C:
```bash
pio run -e default --target upload
```

---

## Architecture

The project follows a modular architecture designed to separate hardware concerns from application logic.

### 1. Activity Framework (`src/activities/`)
The UI is built using an **Activity Pattern** (similar to Android's activity stack). An `ActivityManager` manages a stack of screens, handling navigation, transitions, and lifecycle events (e.g., `onCreate`, `onResume`, `onPause`).

### 2. Hardware Abstraction Layer (HAL) (`lib/hal/`)
The HAL decouples the firmware from the specific ESP32-C3 hardware, abstracting Display, Storage, Power, and System utilities.

### 3. Rendering Engine (`lib/GfxRenderer/`)
A custom graphics engine responsible for:
- **Text Reflow**: Dynamically calculating line breaks and page layouts.
- **Font Rendering**: Using `EpdFont` for compressed font storage and glyph rendering.
- **Image Support**: Converting JPEG/PNG to bitmapped formats.

---

## Dataflow

### 1. Boot & Initialization
1. `main.cpp` initializes the HAL.
2. Global `CrossPointSettings` are loaded from `settings.json` on the SD card.
3. The `ActivityManager` starts the initial `HomeActivity`.

### 2. Content Loading & Caching
To maintain a low RAM footprint, CrossPoint uses an aggressive caching strategy:
1. **Metadata Parsing**: `lib/Epub/` extracts metadata (title, author, cover).
2. **SD Caching**: Metadata and chapter pointers are cached in binary format under `.crosspoint/` on the SD card.
3. **Lazy Loading**: Only the currently needed chapter is loaded into RAM. Page layout is calculated on-the-fly or cached for fast page turns.

---

## Libraries & Dependencies

### Internal Libraries (`lib/`)
- `Epub`: Core EPUB 2/3 parser.
- `EpdFont`: Compressed font format and rendering logic.
- `GfxRenderer`: Text and image rendering pipeline.
- `I18n`: Internationalization and translation support.
- `KOReaderSync`: Client for KOReader progress synchronization.

### External & Third-Party
- **SDK**: `open-x4-sdk` (Hardware drivers for Battery, Input, Display, SD).
- **XML Parsing**: `expat` (embedded version).
- **Compression**: `uzlib` / `ZipFile` (for EPUB/ZIP extraction).
- **JSON**: `ArduinoJson` (v7.x).
- **Image Decoding**: `PNGdec`, `JPEGDEC`, `picojpeg`.
- **QR Codes**: `QRCode`.

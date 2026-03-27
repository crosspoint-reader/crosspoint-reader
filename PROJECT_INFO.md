# Project Information: CrossPoint Reader

CrossPoint Reader is an open-source, minimalist firmware specifically designed for the **Xteink X4** e-reader, which is powered by an **ESP32-C3** microcontroller. The project prioritizes efficiency, focus, and a small RAM footprint (~380KB available) while providing a high-quality **EPUB** and **PDF** reading experience (PDF support is documented in [PDF.md](./PDF.md)).

## Build Instructions

### Prerequisites
- **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
- Python 3.8+
- Git

### Cloning and Setup
```bash
git clone https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader
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
The UI is built using an **Activity Pattern** (similar to Android's activity stack). An `ActivityManager` manages a stack of screens, handling navigation, transitions, and lifecycle events (e.g., `onCreate`, `onResume`, `onPause`). Reader flows include dedicated activities for **EPUB** and **PDF** (e.g. `PdfReaderActivity`, chapter/outline selection where applicable).

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

For **PDF** files, `lib/Pdf/` parses the document structure (classic and streamed xrefs, object streams), decodes page content streams, and caches laid-out pages under `.crosspoint/` similarly to EPUB sections. See [PDF.md](./PDF.md) for architecture and limitations (e.g. text extraction vs CID fonts).

---

## Libraries & Dependencies

### Internal Libraries (`lib/`)
- `Epub`: Core EPUB 2/3 parser.
- `Pdf`: PDF xref, object streams, page tree, outline, and content-stream parsing; integrates with `GfxRenderer` for the PDF reader UI.
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

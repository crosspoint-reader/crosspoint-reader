# CrossPoint Reader with PDF Support

CrossPoint Reader with PDF Support is a fork of the original **CrossPoint / Xteink X4 e-book reader firmware**.
It keeps the open firmware foundation and adds a practical PDF reading path for the device.

![](./docs/images/cover.jpg)

## What It Adds

- PDF reading with text extraction, basic markup, page rendering, bookmarks, and table-of-contents navigation
- Cached PDF loading to reduce repeated parsing
- PDF reader stability improvements for large and complex documents
- EPUB reading optimizations for faster, more reliable rendering and navigation

## Other Features of CrossPoint

- EPUB reading for the Xteink X4
- Saved reading position
- File browser, Wi-Fi book upload, and OTA updates
- Configurable font, layout, rotation, and display options
- KOReader Sync integration

PDF ingestion, xref/object-stream handling, and content-stream parsing are described in [PDF.md](./PDF.md).
Host-side parser checks can be run with:

```sh
bash test/pdf/run_pdf_parser_tests.sh
```

## Installation

1. Download the latest `firmware.bin` from the [releases page](https://github.com/ioma8/crosspoint-ioma8/releases)
2. Connect your Xteink X4 to your computer via USB-C and wake/unlock the device
3. Open https://xteink.dve.al/ and flash the firmware file with the OTA flash controls

The release page provides a single firmware binary for the current build.

### Manual

Build and flash locally with PlatformIO using the same workflow as the upstream project.

---

CrossPoint Reader with PDF Support is not affiliated with Xteink or any manufacturer of the X4 hardware.

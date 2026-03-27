# CrossPoint Reader with PDF Support

CrossPoint Reader with PDF Support is a fork of the original **CrossPoint / Xteink X4 e-book reader firmware**.
It keeps the open firmware foundation and adds a practical PDF reading path for the device.

![](./docs/images/cover.jpg)

## What It Adds

- EPUB reading for the Xteink X4
- PDF support with text extraction, basic markup, page rendering, and bookmarks
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

### Web (latest firmware)

1. Connect your Xteink X4 to your computer via USB-C and wake/unlock the device
2. Go to https://xteink.dve.al/ and click "Flash CrossPoint firmware"

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Web (specific firmware version)

1. Connect your Xteink X4 to your computer via USB-C
2. Download the `firmware.bin` file from the release of your choice via the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases)
3. Go to https://xteink.dve.al/ and flash the firmware file using the "OTA fast flash controls" section

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Manual

Build and flash locally with PlatformIO using the same workflow as the upstream project.

---

CrossPoint Reader with PDF Support is not affiliated with Xteink or any manufacturer of the X4 hardware.

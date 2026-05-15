# Marginalia Firmware

Firmware for ESP32-C3-based Xteink X3/X4 e-paper reader devices. Marginalia is a community fork of CrossPoint Reader
that keeps the reader base and adds a package ecosystem for modules, themes, sleep-screen experiences, integrations,
and standalone apps.

Marginalia is not affiliated with Xteink or any device manufacturer.

![Marginalia firmware running on Xteink device](./docs/images/cover.jpg)

## What Marginalia Does

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter
  navigation, footnotes, go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync, and
  more.
- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.
- **Screenshots**.
- **Custom fonts**: install your favorite fonts on the SD card.
- **Tilt page turn** on X3.
- **Library workflow**: folder browser, hidden-file toggle, long-press delete, recent books, and SD-cache management.
- **Wireless workflows**: file transfer web UI, EPUB optimizer, web settings UI/API, WebSocket fast uploads, WebDAV,
  AP/STA Wi-Fi modes with QR helpers, Calibre wireless connect flow, OPDS browsing, and OTA updates.
- **Customization**: themes, sleep screen modes, front/side button remapping, status bar controls, power-button
  behavior, refresh cadence, and more.
- **Extensions**: side-loaded package folders and SDK-built `.mpkg.zip` archives with a manifest, compatibility model,
  hub catalog path, lifecycle state, and extension settings.
- **Localization**: 22 UI languages and counting.

Coming soon:

- RTL support for Arabic, Hebrew, and Farsi.
- Bookmarks.
- Dictionary lookup.
- More themes.

See [the user guide](./USER_GUIDE.md) for instructions on operating Marginalia, including the
[KOReader Sync quick setup](./USER_GUIDE.md#367-koreader-sync-quick-setup).

For more details about Marginalia's reader-first package direction, see [SCOPE.md](SCOPE.md). Ecosystem notes live under
`docs/marginalia/`.

## USB-Locked Devices

Some Xteink units purchased from third-party stores, including some AliExpress units, ship with USB flashing locked from
the factory. If your device is locked, you may need the **Xteink Unlocker** tool at
https://crosspointreader.com/#unlock-tool before USB flashing works.

You do not need this tool if you bought your device directly from xteink.com. Those units are not locked.

Not sure if your device is locked? Power it on, connect the USB-C cable, and try flashing via the web flasher first. If
the browser's serial device picker does not show your device, try a different USB port or browser before assuming the
device is locked.

The unlocker is maintained by the CrossPoint project and its officially supported firmware choices may differ from this
fork. Do not flash firmware without a working OTA or recovery path on a USB-locked device.

## Install Firmware

### Web Installer

1. Connect your device to your computer via USB-C and wake/unlock the device.
2. Download the `firmware.bin` file from the release or build artifact you want to install.
3. Go to https://xteink.dve.al/ and flash the firmware file using the OTA fast flash controls.

### Command Line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download the `firmware.bin` file from the release or build artifact you want to install.
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

Adjust `/dev/ttyACM0` to match your system.

### Revert to Official Firmware

To revert to the official firmware, flash the latest official firmware from https://xteink.dve.al/, or swap back to the
other partition using the "Swap boot partition" button at https://xteink.dve.al/debug.

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)

## Development Quick Start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/marginalia-os/marginalia-firmware
cd marginalia-firmware

# if cloned without --recursive:
git submodule update --init --recursive
```

### Build / Flash / Monitor

```bash
pio run --target upload
```

### Contributor Pre-PR Checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing new features, it is useful to capture detailed logs from the serial port.

First, install the required Python packages:

```bash
python3 -m pip install pyserial colorama matplotlib
```

Then run the script:

```sh
# Linux
python3 scripts/debugging_monitor.py

# macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

## Internals

Marginalia is aggressive about caching data to the SD card to minimize RAM usage. The ESP32-C3 only has roughly 380KB of
usable RAM, so firmware design still needs to respect that constraint.

### Data Caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the cache.
This cache directory exists at `.crosspoint` on the SD card:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position: chapter, page, etc.
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. The cache is not cleared
automatically when you delete a book, and moving a file to a new path resets its reading progress.

### Packages

Marginalia scans side-loaded packages from `/.marginalia/packages/*/manifest.json` on the SD card. Package folders and
SDK-built `.mpkg.zip` archives can be uploaded from the local web package manager, and compatible hub entries can be
downloaded from the package page. Archives are unpacked into the package inbox before install. Runtime hooks and app
launching will be added on top of this store instead of being mixed into the package installer.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

## Contributing

Contributions are welcome. If you are new to the codebase, start with the
[contributing docs](./docs/contributing/README.md). For things to work on, check the
[ideas discussion board](https://github.com/marginalia-os/marginalia-firmware/discussions/categories/ideas) and leave a
comment before starting so work is not duplicated.

For the ecosystem direction, start with [docs/marginalia/architecture.md](./docs/marginalia/architecture.md).

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see
[GOVERNANCE.md](./GOVERNANCE.md).

Huge shoutout to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired CrossPoint
and therefore this fork.

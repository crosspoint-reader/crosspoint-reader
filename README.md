# CrossVi

[**English**](README.md) | [Tiếng Việt](README.vi.md)

[![Support upstream CrossPoint contributors](https://img.shields.io/badge/Support_upstream-CrossPoint-BB953A?style=for-the-badge&labelColor=1a1a1a)](https://app.royalty.dev/crosspoint-reader/crosspoint-reader)

CrossVi is an independent fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) for readers and contributors worldwide. It keeps the open, hackable reading experience for Xteink e-paper devices while focusing on faster text layout, safer SD-card I/O, resilient caches, and carefully developed new features.

The original CrossPoint project and community remain the foundation of this firmware. CrossVi preserves its MIT license, device support, technical identifiers, protocols, and on-card `.crosspoint` data layout. New protected formats are written beside retained legacy data so migration remains non-destructive.

> **Current validation status:** core codecs, storage recovery, layout mapping, and protocol state machines have host tests, static analysis, and ESP32-C3 build validation. End-to-end UI, ESP-NOW radio, e-paper refresh, power-loss behavior, and memory use still require testing on physical X3/X4 hardware.

**Target devices:** ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3).

![CrossPoint Reader on an Xteink device (upstream project image)](./docs/images/cover.jpg)

> If you're planning to buy an Xteink device, consider purchasing an **X3/X4 Developer Edition** through https://crosspointreader.com. CrossPoint receives a small share of each sale, helping fund development costs.

## What can CrossVi do?

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter navigation, footnotes, bookmarks, dictionary lookups ([StarDict](docs/dictionary.md)), go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more. 

- **EPUB reading tools**: create and revisit clippings, view per-book and all-time reading statistics, and keep a separate reader profile for each EPUB without changing the device-wide defaults.

- **Dashboard home theme**: a compact overview of the latest book and reading totals. Enable it under **Settings → Display → UI Theme → Dashboard**.

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Screenshots.**

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 only)**.

- **Library workflow**: folder browser, hidden-file toggle, long-press delete, recent books, SD-cache management.

- **Wireless workflows**:
  
  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download
  - OTA update checks and installs from GitHub releases
  - Experimental Nearby Sync between two CrossVi readers: manually exchange an exact-book reading position or a separate reading-statistics snapshot without an internet connection

- **Customization**: multiple themes (Classic, Lyra, Lyra Extended, RoundedRaff), sleep screen modes, front/side button remapping, status bar controls, power-button behavior, refresh cadence, and more.

- **Localization**: 30 UI languages and counting, including Vietnamese. RTL support. New reader tools are fully translated in English and Vietnamese; other languages currently use English fallback for some new labels.

> **Safety boundaries for the new reading tools:** clipping selection can continue across adjacent rendered pages in the same chapter, within fixed memory and text limits; nothing is saved until the reader confirms the complete selection. A saved highlight or jump is applied only when its source and layout can be matched safely. Nearby position sync requires the same complete EPUB file, a usable paragraph anchor in the target chapter's current local layout, and explicit confirmation on both devices; it refuses to guess a page when those checks are unavailable. Nearby traffic is not encrypted, so use it only with a trusted reader nearby.

### Coming soon:

- More themes.

- Much more! stay tuned.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you will need to use the **Xteink Unlocker** tool available at
https://crosspointreader.com/#unlock-tool before you can flash a supported firmware.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

> ### ⚠️ WARNING: READ THIS BEFORE USING THE UNLOCKER ⚠️
> 
> **The only officially supported firmwares in the unlock tool are CrossPoint and CrossInk.**
> 
> **CrossVi is an independent fork and is not currently on that supported list. Do not use it as the unlock payload for a USB-locked device.**
>
> Flashing any other firmware on a USB-locked device may **permanently brick the device** or leave it **permanently
> stuck on that firmware with no recovery path**. Once USB flashing is re-locked, your only way back is via OTA, and if
> the firmware you flashed doesn't support OTA, **there is no way out**.

## Install firmware

### Web installer

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Download `firmware.bin` from the [CrossVi Releases page](https://github.com/tvhdc/crossvi/releases), a local build, or a continuous integration artifact.
3. Go to https://crosspointreader.com/#flash-tools, select device (X3 or X4), click "Custom .bin", and upload `firmware.bin`.

> CrossVi does not yet publish a hardware-validated release. Do not flash it to a USB-locked device.

### Revert to official CrossPoint firmware

To revert to the official firmware, you can also flash the latest official firmware using https://crosspointreader.com/#flash-tools.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download `firmware.bin` from the [CrossVi Releases page](https://github.com/tvhdc/crossvi/releases).
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

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

Convert your own TTF/OTF files into `.cpfont` files that load from the SD card. No firmware reflash is needed.

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)

---

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/tvhdc/crossvi
cd crossvi

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossVi inherits CrossPoint Reader's aggressive SD-card caching to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### SD-card working data

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. The same `.crosspoint` directory also contains settings and reader data that are **not disposable cache**. Its main structure is:

```text
.crosspoint/
├── epub_<path-hash>/    # one directory per book path
│   ├── progress.bin     # saved reading position
│   ├── crossvi_reader_settings.bin  # per-book reader profile
│   ├── stats_v6.bin     # CRC-protected per-book reading statistics
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # generated title, author, spine, and TOC metadata
│   ├── css_rules.cache  # generated parsed-CSS cache
│   ├── img_*            # generated image cache files
│   └── sections/        # generated per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── bookmarks/           # bookmark JSON files
├── clippings/           # saved EPUB passages
├── synced_stats/        # CRC-protected per-device Nearby snapshots
├── global_stats_v4.bin  # CRC-protected local all-time reading statistics
├── settings.bin/json    # device settings and compatibility fallback
├── state.bin/json       # resume/runtime state and compatibility fallback
└── recent.json          # recent books list
```

CrossVi keeps supported `stats_v5.bin`/`stats_v4.bin`/`stats.bin` and
`global_stats.bin` files unchanged when it migrates them. The new envelope is
written beside the legacy source so an interrupted migration cannot destroy the
only copy and older firmware data remains available for manual rollback.

Do **not** delete all of `/.crosspoint` as routine cache troubleshooting: that is effectively a reader-data reset and removes settings, positions, bookmarks, clippings, and statistics. Use the firmware's cache command first. It preserves the saved position, per-book profile, clippings, and reading statistics while rebuilding only generated indexes and render data. For the narrowest manual layout reset, back up the SD card and move only that book's `sections/` directory aside. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching state; manual SD-card edits may leave stale path-keyed directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the [contributing docs](./docs/contributing/README.md). For things to work on, check the [CrossVi ideas board](https://github.com/tvhdc/crossvi/discussions/categories/ideas) — leave a comment before starting so we don't duplicate effort.

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

## Community forks

One of the best things about open source is that anyone can take the code in a different direction. If you need something outside CrossVi's [scope](./SCOPE.md), check out these sibling CrossPoint forks:

- [CrossInk](https://github.com/uxjulia/CrossInk) — Its Dashboard, clipping, Nearby Sync, reading-statistics, and per-book-settings work informed CrossVi's independently reviewed implementations. CrossInk also explores Bionic Reading, guide dots, paragraph typography, and alternative fonts.

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) — Adds FB2 and MD format support. Actively maintained with Arabic script support. Custom themes via SD card.

- ~~[crosspet](https://github.com/trilwu/crosspet) — A Vietnamese fork that adds a Tamagotchi-style virtual chicken that grows based on your reading milestones (pages read, streaks, care). Also: Flashcards, Weather, Pomodoro timer, and mini-games.~~ (Unmaintained)

- [crosspoint-reader-cjk](https://github.com/aBER0724/crosspoint-reader-cjk) — Purpose-built for Chinese, Japanese, and Korean reading.

- [inx](https://github.com/obijuankenobiii/inx) — Completely reimagines the user interface with tabbed navigation.

- ~~[PlusPoint](https://github.com/ngxson/pluspoint-reader) — custom JS apps support.~~ (Unmaintained)

- [crosspoint-reader-papers3](https://github.com/juicecultus/crosspoint-reader-papers3) — Crosspoint port for M5Stack Paper S3. 

- [t5s3-reader](https://github.com/ShallowGreen123/t5s3-reader) — Crosspoint port for LilyGo T5 ePaper S3 / T5S3 4.7-inch e-paper device.

**Note:** Some of these features may reach upstream CrossPoint or CrossVi over time. CrossVi favours stability before changes reach devices.

Want to build your own device? Be sure to check out the [de-link](https://github.com/iandchasse/de-link) project.

---

CrossVi is an independent fork of CrossPoint Reader, developed as an international open-source project. It is **not affiliated with Xteink or any device manufacturer**.

Huge shoutout to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired this project.

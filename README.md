# CrossPoint Reader

[![Fund contributors](https://img.shields.io/badge/%F0%9F%91%91_Fund_contributors-royalty.dev-BB953A?style=for-the-badge&labelColor=1a1a1a)](https://app.royalty.dev/crosspoint-reader/crosspoint-reader)

CrossPoint is free, community-made software for supported e-readers. It lets you read books from the SD card and change how the reader works.

### Supported devices

- Xteink X4 and X3.
- Xteink X4Pro, Seeed reTerminal Sticky, and M5PaperMono.

Check [our Devices page](https://crosspointreader.com/devices) for the full list.

![CrossPoint Reader running on Xteink device](./docs/images/cover.jpg)

> If you plan to buy an Xteink device, consider an **X3/X4 Developer Edition** from https://crosspointreader.com. CrossPoint receives part of each sale.

## What you can do

- Read EPUB 2 and EPUB 3 books. CrossPoint supports chapter navigation, footnotes, bookmarks, dictionary lookup, automatic page turns, screen rotation, focus reading, and KOReader progress sync.

- Open `.epub`, `.xtc`, `.xtch`, `.txt`, and `.bmp` files.

- Follow links and look up words on touch-enabled devices.

- Save screenshots.

- Install fonts from the SD card.

- Turn pages by tilting an X3 or Sticky.

- Use USB Drive mode on X4Pro to access the SD card from a computer.

- Find books by title or author, browse folders, and reopen recent books.

- Transfer books wirelessly:
  
  - Use the file-transfer page in a browser.
  - Join Wi-Fi or create a hotspot.
  - Send books from Calibre.
  - Browse and download books from up to eight OPDS catalogs.
  - Install firmware updates over Wi-Fi.

- Choose a theme, sleep screen, button layout, status bar, power-button behavior, and screen-refresh interval.

- Choose from 34 interface languages. You can install fonts for Chinese, Japanese, Korean, right-to-left languages, and other scripts.

### Planned features

- More themes.

- Web plugins.

- Bluetooth pageturner.


---

## If your Xteink device is USB-locked

Some Xteink units from third-party stores have USB installation locked. If your device is locked, use the **Xteink Unlocker** at https://crosspointreader.com/#unlock-tool before you install CrossPoint.

You do not need this tool if you bought the device directly from xteink.com.

If you do not know whether it is locked, try the web installer first. If the browser does not list the device, try another USB port or browser. Use the unlocker only if it still does not appear.

> ### Warning
> 
> Use the unlocker only with CrossPoint or CrossInk firmware.
> 
> Do not use other firmware. It can permanently stop the device from working or leave it on firmware that you cannot replace.

## Install CrossPoint

### Use the web installer

1. Connect the device to your computer with a USB-C data cable. Wake or unlock the device.
2. Go to https://crosspointreader.com/#flash-tools.
3. Select your device and an official CrossPoint release.

### Install a specific version

1. Download the matching firmware file from [Releases](https://github.com/crosspoint-reader/crosspoint-reader/releases).
2. Connect and wake the device.
3. Open https://crosspointreader.com/#flash-tools, select the device, choose **Custom .bin**, and select the file.

### Return to the official firmware

Use https://crosspointreader.com/#flash-tools to install the latest official firmware.

### Advanced: command line installation

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download the firmware file for your device from the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash an X3 or X4:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

   Flash an Xteink X4Pro, Seeed reTerminal Sticky, or M5PaperMono:

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

### Build it yourself

See [Development quick start](#development-quick-start). This section is for contributors.

---

## Make a custom font

You can convert TrueType (`.ttf`) or OpenType (`.otf`) fonts for the SD card. You do not need to reinstall CrossPoint.

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles. Set the family name, text sizes, and character range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font in the reader font settings.

The web tool uses the same conversion tool as the firmware project.

---

## Guides for readers

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Dictionary setup](./docs/dictionary.md)
- [SD card fonts](./docs/sd-card-fonts.md)
- [Troubleshooting](./docs/troubleshooting.md)
- [Recover a bricked Xteink](./docs/fix-bricked-xteink.md)

## Developer documentation

- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)
- [Touch and UI development](./docs/contributing/touch-and-ui.md)

---

## Development quick start

### Prerequisites

- [pioarduino PlatformIO Core](https://github.com/pioarduino/platformio-core) or [VS Code + pioarduino IDE](https://github.com/pioarduino/pioarduino-vscode-ide)
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader

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

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the [contributing docs](./docs/contributing/README.md). For things to work on, check the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas) — leave a comment before starting so we don't duplicate effort.

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

## Community forks

One of the best things about open source is that anyone can take the code in a different direction. If you need something outside CrossPoint's [scope](./SCOPE.md), check out the community forks:

- [CrossInk](https://github.com/uxjulia/CrossInk) — UX focused with minimal reading stats and broader customizations for the reading experience.

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) — Adds FB2 and MD format support. Actively maintained with Arabic script support. Custom themes.

- [inx](https://github.com/obijuankenobiii/inx) — Completely reimagines the user interface with tabbed navigation.

- [Witch(hunt) Reader](https://github.com/jpirnay/witchhunt-reader) — More faithful CSS styling and background work for slightly snappier interaction. Weather information panel. Markdown support.

**Note:** Many of these features will make their way into CrossPoint over time. Each project chooses its own priorities and tradeoffs.

Want to build your own device? Be sure to check out the [de-link](https://github.com/iandchasse/de-link) project or [OnePage Reader](https://github.com/MoveCall/onepage-reader).

---

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.

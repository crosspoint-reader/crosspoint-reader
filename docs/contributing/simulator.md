# X3/X4 desktop UI simulator

CrossVi includes an interactive SDL2 simulator for testing the firmware UI
without an Xteink device. It compiles and runs the real CrossVi `setup()` and
`loop()`, Activities, `GfxRenderer`, built-in fonts, icons, themes, EPUB image
decoders, and input mapping. The simulator replaces only hardware-facing code
such as the e-paper driver, GPIO, SD card, power control, and radio.

![CrossVi X4 simulator with the pixel-exact firmware viewport and clickable controls](../images/crossvi-simulator-x4.png)

## Start it

Install PlatformIO Core first. On Debian, Ubuntu, or Linux Mint, the launcher
can download SDL2/OpenSSL development files into the ignored `.cache` directory
without administrator access. You can instead install them system-wide:

```sh
sudo apt-get install libsdl2-dev libssl-dev
```

On macOS, install the native dependencies with Homebrew:

```sh
brew install sdl2 openssl@3 pkg-config
```

The simulator currently targets POSIX desktops (Linux and macOS); Windows is
not supported by the host storage and process adapters yet.

Run the model you want to inspect:

```sh
python3 scripts/run_simulator.py x3
python3 scripts/run_simulator.py x4
```

The first launch builds the native target. Later launches are incremental.
Use `--build-only` when you only want to compile it.

## Buttons

Every visible button in the dark control strip is clickable. Mouse presses,
releases, and holds are sent through the same raw input indices used on the
device, so button remapping and orientation logic still apply.

| Firmware input | Keyboard |
| --- | --- |
| Back | `Esc` |
| Confirm | `Enter` |
| Left | `Left Arrow` |
| Right | `Right Arrow` |
| Up / previous page | `Up Arrow` |
| Down / next page | `Down Arrow` |
| Power | `P` |
| Enter simulated sleep | `S` |
| Capture framebuffer | `F12` |

The control strip is outside the firmware framebuffer and never appears in a
captured UI image.

For deterministic automation, `CROSSVI_SIM_INPUT_SCRIPT` accepts comma-separated
`time-ms:key` events. Add a third field to hold a key before releasing it, for
example `1200:CONFIRM,2600:RIGHT,4000:POWER:800`. Supported names are `BACK`,
`CONFIRM`, `LEFT`, `RIGHT`, `UP`, `DOWN`, `POWER`, `SLEEP`, and `SCREENSHOT`.
`CROSSVI_SIM_EXIT_ON_SLEEP=1` makes a scripted deep-sleep transition terminate
the host process after firmware state has been saved. These variables are for
tests; interactive use should normally go through the launcher above.

## Virtual SD card and books

Each model has isolated data by default:

```text
.simulator-data/x3/
.simulator-data/x4/
```

Copy EPUB, TXT, XTC/XTCH, or BMP files into the selected directory before
launch, then use **Browse Files** exactly as on the reader. To use another
directory:

```sh
python3 scripts/run_simulator.py x4 --sd /absolute/path/to/test-library
```

Do not point the simulator at the only copy of a real SD card. It runs real
CrossVi persistence and cache code and can modify the selected directory.

## Pixel captures

Press `F12` at any screen. CrossVi writes:

- a logical-orientation `.bmp` containing only the firmware UI;
- a physical one-bit `.framebuffer.bin` suitable for byte/hash comparisons.

The default output is `simulator-screenshots/`. Choose another location with:

```sh
python3 scripts/run_simulator.py x3 --screenshot-dir /tmp/crossvi-shots
```

Expected portrait captures are exactly `528x792` for X3 and `480x800` for X4.
The raw buffers are 52,272 and 48,000 bytes respectively.

## What "pixel accurate" means

The X3 and X4 targets use their real, distinct framebuffer geometry. Rendering
uses CrossVi's production code and nearest-neighbour presentation; no desktop
font, HTML recreation, or scaled X4 mock is involved. A mutex protects the
presented frame from tearing while screenshots are taken. This makes the
simulator suitable for pixel-level layout, typography, clipping, focus,
navigation, and visual-regression work.

It does not reproduce the physical appearance or timing of e-paper. Ghosting,
waveforms, refresh duration, ADC button thresholds, deep sleep current, flash
limits, SD electrical failures, Wi-Fi, ESP-NOW/Nearby Sync, and OTA behavior
still require real X3/X4 hardware. Absolute parity with a panel can only be
confirmed later by comparing the simulator raw buffer with a device capture
from the same build and state.

## Regression checks

Run both native builds, control/input/storage adapter tests, boot-to-Home smoke
tests, exact dimension checks, and the empty-library raw-framebuffer plus
logical-BMP golden hashes with:

```sh
python3 scripts/test_simulator.py
```

The same command opens the frozen converter XTC and XTCH fixtures through File
Browser on both models. It checks the X4 page pixel-for-pixel, checks the X3
fit/centering/letterbox mapping pixel-for-pixel, verifies all four XTCH gray
levels, hidden/top/bottom status overlays, progress reload, cover, Home
thumbnail, sleep cover, and exercises a generated three-page/two-chapter book
through Forward, Back, and chapter jump.
The generated book is derived from the real converter fixture and is written
only under the ignored `build/simulator-tests/` directory. An additional X3
stress run injects 1,000 alternating page-turn requests and verifies that the
final bitmap and persisted progress still identify the same page. A replacement
regression also swaps in a different XTC with the same path, byte size, and page
count, then verifies that progress, source identity, cover, and thumbnail state
from the old book are not reused.

If an intentional UI change alters the empty Home frame, review both generated
captures under `build/simulator-tests/` and then update the accepted hashes:

```sh
python3 scripts/test_simulator.py --skip-build --update-golden
```

Do not update a golden hash merely to make CI pass; inspect the X3 and X4 images
first.

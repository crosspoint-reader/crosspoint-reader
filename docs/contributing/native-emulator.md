# Native Emulator

The native emulator builds a desktop SDL app for fast local testing of UI and reader changes without flashing a device.
It is intended as a development aid, not a replacement for hardware validation.

## Prerequisites

- PlatformIO Core
- SDL2 development files

On macOS with Homebrew:

```sh
brew install sdl2
```

## Build

```sh
pio run -e emulator
```

The emulator binary is written to:

```sh
.pio/build/emulator/program
```

## Run

The emulator maps the SD card to a local directory. By default it uses `./emu_sd`.
You can also pass an explicit path:

```sh
CROSSPOINT_EMU_SD="$PWD/emu_sd" .pio/build/emulator/program
```

The helper script creates the expected SD folders and runs the emulator with the local SD root:

```sh
scripts/run_emulator.sh
```

## Add Books

Put books under the `books` directory inside the emulated SD root:

```sh
mkdir -p emu_sd/books
cp /path/to/book.epub emu_sd/books/
```

Then start the emulator and open the book from the Books screen.

## Controls

- Arrow keys: directional buttons
- Enter or Space: confirm
- Escape or Backspace: back
- Tab or `p`: power button

Press and release Tab to enter the sleep screen. Press and release Tab again to wake back to the home screen.

## Current Limitations

- Wi-Fi, OPDS, OTA update, browser, and file-transfer flows are stubbed.
- The emulator renders a monochrome approximation of the e-paper framebuffer.
- Grayscale refresh is not fully simulated yet.
- Final validation should still be done on Xteink X4 hardware.

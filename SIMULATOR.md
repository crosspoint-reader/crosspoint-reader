# CrossPoint Reader Simulator

The CrossPoint Reader simulator allows you to test the firmware on your desktop (macOS or Linux) without needing a physical device. The simulator compiles the firmware natively and renders the e-ink display in an SDL2 window.

## Quick Start

### One-Command Build & Run
```bash
./build-and-test.sh
```

Runs the quality gates, builds `simulator_x4_pro`, and launches it. The
simulator window will open and you can start testing immediately!

Other modes — `check` (gates only, seconds), `all` (every gate and every
environment CI builds, before you push), `run` (relaunch the existing binary),
`device`, `clean`. Run `./build-and-test.sh --help` for the list.

Without the script:

```bash
pio run -e simulator_x4_pro && .pio/build/simulator_x4_pro/program
```

## Prerequisites

### macOS
```bash
brew install sdl2
```

### Linux / WSL
```bash
sudo apt install libsdl2-dev
```

### Rust (Required for building Rust components)
```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Verify installation:
```bash
rustc --version && cargo --version
```

## Building the Simulator

### Option 1: Build for X4 Pro (Recommended for testing)
```bash
pio run -e simulator_x4_pro
```

This creates a binary targeting the X4 Pro device profile with:
- 800x480 display
- Touch input support
- Capacitive Home key
- Frontlight panel (cold/warm)
- 8MB PSRAM

### Option 2: Build for X3
```bash
pio run -e simulator_x3
```

This creates a binary for the X3 device with:
- 792x528 display (landscape)
- Tilt sensor
- Single frontlight channel

### Option 3: Build default simulator (X4)
```bash
pio run -e simulator
```

## Running the Simulator

### Launch the simulator binary
```bash
.pio/build/simulator_x4_pro/program
```

This will:
1. Open an SDL2 window (800x480) showing the e-reader interface
2. Display console output in the terminal with debug logs
3. Show the home screen with book browser

The window will remain open and responsive to input until you close it or press Ctrl+C.

## Simulator Controls

### Keyboard Mapping
| Key | Action |
|-----|--------|
| ↑↓←→ | Navigate menus |
| Enter | Confirm selection |
| Escape | Back button / Go back |
| T | Toggle frontlight panel (X4 Pro only) |
| L | Toggle display inversion |
| Q | Quit simulator |

### Mouse (X4 Pro only)
- **Click**: Touch input
- **Drag**: Swipe gestures

## Testing the Rust Integration (About Screen)

The simulator includes a new Rust-based "About" screen, demonstrating successful C++ ↔ Rust FFI integration:

### Navigate to About Screen
1. Press **Down Arrow** → Settings
2. Press **Right Arrow** → System tab
3. Press **Enter** → About
4. View version/device info from Rust! ✅

### What You'll See
- **Version**: "CrossPoint 1.5.0-x4pro" (from Rust)
- **Device Info**: "Device: Xteink X4 Pro | ESP32-S3 with 8MB PSRAM" (from Rust)
- Confirmation: "PoC: Rust + C++ FFI integrating Rust with C++"

### What This Demonstrates
- ✅ Rust code compiled successfully
- ✅ C++ can call Rust FFI functions
- ✅ String data flows correctly between languages
- ✅ UI rendering works with Rust-provided content

## Understanding Simulator Output

When running, you'll see debug logs like:
```
[0] [DBG] [UI] Using Lyra theme
[252] [INF] [MAIN] Device: xteink_x4_pro
[253] [DBG] [MAIN] Starting CrossPoint version dev-simulator
[682] [DBG] [MAIN] Display initialized
[682] [DBG] [MAIN] Fonts setup
```

Each line shows:
- **[timestamp]**: Milliseconds since startup
- **[LOG_LEVEL]**: DBG (debug), INF (info), ERR (error)
- **[MODULE]**: Which subsystem logged (MAIN, UI, GFX, etc.)
- **Message**: The actual log message

## Development Workflow

### Recommended Workflow for Testing Features

1. **Make code changes** (C++ or Rust)
2. **Build simulator**: `pio run -e simulator_x4_pro`
3. **Run and test**: `.pio/build/simulator_x4_pro/program`
4. **Verify in simulator** (no device flashing needed)
5. **When ready**, test on hardware: `pio run -e x4pro -t upload`

### Fast Iteration Loop
```bash
# Gates, build and run in one step
./build-and-test.sh

# Gates only - no build, a few seconds
./build-and-test.sh check
```

### Clean Build (if experiencing issues)
```bash
./build-and-test.sh clean
```

That wipes `.pio/build` **and** `target/`. Dropping `target/` forces a full
Rust rebuild including `build-std` for Xtensa, which costs minutes — only worth
it when cargo itself is misbehaving. For a C++-only problem, this is enough:

```bash
rm -rf .pio/build/simulator_x4_pro/
pio run -e simulator_x4_pro
```

### Full Cleanup
```bash
# Clean build
pio run -e simulator_x4_pro -t clean && pio run -e simulator_x4_pro

# Remove all build artifacts
rm -rf .pio/build/simulator_x4_pro/
```

## Troubleshooting

### "SDL2 not found" error

**macOS**: Install SDL2
```bash
brew install sdl2
```

**Linux**: Install SDL2 dev headers
```bash
sudo apt install libsdl2-dev
```

Then rebuild:
```bash
pio run -e simulator_x4_pro -t clean
pio run -e simulator_x4_pro
```

### Window doesn't appear
- Check that SDL2 is properly installed
- Try running with verbose output: `pio run -e simulator_x4_pro -v`
- On some Linux systems, you may need to set `DISPLAY` environment variable if running remotely

### Rust compilation errors
The Rust crates are built automatically during the PlatformIO build by
`scripts/build_rust.py`. See [docs/rust-ui-framework.md](docs/rust-ui-framework.md).

If it fails:
```bash
# Check the Rust gates directly for a clearer error
./build-and-test.sh check

# Clean Rust artifacts and rebuild
rm -rf target
pio run -e simulator_x4_pro
```

### Simulator exits immediately
- Check the console output for error messages
- Look for missing resource files (fonts, images)
- Try with clean build: `pio run -e simulator_x4_pro -t clean && pio run -e simulator_x4_pro`

## Known Limitations

1. **No SD Card**: The simulator uses a local `fs_/` directory instead of SD card
2. **No WiFi**: Network features are stubbed (no actual downloads)
3. **No Hardware buttons**: Only keyboard controls work
4. **Partial Touch**: X4 Pro touch is simulated via mouse, not full gesture library
5. **No OTA Updates**: Firmware update features are disabled in simulator

## Important Paths

- **Binary**: `.pio/build/simulator_x4_pro/program`
- **Simulated SD card**: `fs_/` (local directory)
- **Rust framework**: `lib/xpui/`
- **Rust screens**: `lib/crosspoint_rs/src/activities/`
- **About screen**: `lib/crosspoint_rs/src/activities/settings/about.rs`
- **C++ bridge**: `src/activities/ActivityRs.cpp`, `src/activities/RustActivityStubs.cpp`

## Useful Commands

```bash
# Build only (no run)
pio run -e simulator_x4_pro

# Clean and rebuild
pio run -e simulator_x4_pro -t clean && pio run -e simulator_x4_pro

# Build with verbose output
pio run -e simulator_x4_pro -v

# Monitor serial output (after running)
pio device monitor

# List available environments
pio project config --get platformio --get env
```

## Next Steps

Now that the simulator is working with Rust integration proven:

1. **Add Gesture Detection**: Implement swipe and long-press recognition in Rust
2. **Add Progress Tracking**: Create persistent progress storage with Rust safety guarantees
3. **Test Touch Features**: Use simulator's mouse input to validate gesture detection
4. **Validate on Hardware**: Once simulator testing passes, test on actual X4 Pro device

## Resources

- [SDL2 Documentation](https://wiki.libsdl.org/)
- [Rust FFI Guide](https://doc.rust-lang.org/nomicon/ffi.html)
- [CrossPoint Reader Architecture](./CLAUDE.md)
- [FreeInk SDK](https://freeink.org)

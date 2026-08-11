# CrossPoint Reader Simulator

Builds this firmware as a desktop binary (macOS or Linux) and draws the e-ink
panel in an SDL2 window, so UI work does not need a device on the desk. The
same `src/` compiles unmodified: host stand-ins for the ESP-only headers live
in [src/platform/sim_shims/](../src/platform/sim_shims/).

## Setup

The simulator lives in a **second repository**, so its PlatformIO environments
name a path that differs per machine. They are not in `platformio.ini` — you
add them once to `platformio.local.ini`, which is gitignored and read
automatically.

**1. Install SDL2 and Rust**

```bash
brew install sdl2                  # macOS
sudo apt install libsdl2-dev       # Linux / WSL

curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustc --version && cargo --version
```

**2. Clone the simulator beside this repository**

```bash
git clone https://github.com/crosspoint-reader/crosspoint-simulator.git ../crosspoint-simulator
```

**3. Create `platformio.local.ini`** with the block below. If your checkout is
not a sibling of the simulator — a git worktree, for instance — use an absolute
path instead of `../crosspoint-simulator`.

```ini
[env:simulator]
platform = native
lib_ldf_mode = deep+
build_flags =
  -std=gnu++2a
  !sdl2-config --cflags --libs
  -Wno-c++11-narrowing
  -DSIMULATOR
  -DCROSSPOINT_SIMULATOR_PROJECT_WEBSERVER
  -DCROSSPOINT_VERSION=\"dev-simulator\"
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=2
  -DEINK_DISPLAY_SINGLE_BUFFER_MODE=1
  -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES=1
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  -DUSE_UTF8_LONG_NAMES=1
  -DPNG_MAX_BUFFERED_PIXELS=16416
  -DDISABLE_FS_H_WARNING=1
  -DDESTRUCTOR_CLOSES_FILE=1
  -Isrc
  -Isrc/platform/sim_shims
build_src_filter =
  +<*>
  -<network/FirmwareFlasher.cpp>
  -<network/OtaBootSwitch.cpp>
  -<network/OtaUpdater.cpp>
  -<platform/skip_efuse_blk_check.c>
lib_ignore = hal, PNGdec, JPEGDEC
extra_scripts =
  pre:scripts/gen_i18n.py
  pre:scripts/git_branch.py
  pre:scripts/build_html.py
  pre:scripts/build_rust.py
lib_deps =
  simulator=symlink://../crosspoint-simulator
  FreeInkUI=symlink://freeink-sdk/libs/ui/FreeInkUI
  Icons=symlink://freeink-sdk/libs/assets/Icons
  bblanchon/ArduinoJson @ 7.4.2
  ricmoo/QRCode @ ^0.0.1
  links2004/WebSockets @ 2.7.3

[env:simulator_x3]
extends = env:simulator
build_flags =
  ${env:simulator.build_flags}
  -DSIMULATOR_DEVICE_X3
```

## Build and run

```bash
./build-and-test.sh sim                                  # build and launch
pio run -e simulator_x3 && .pio/build/simulator_x3/program   # the same, by hand
```

`./build-and-test.sh` with no argument runs the quality gates only, in seconds,
and builds nothing. `--help` lists the other modes.

| Environment | Device | Panel | Touch |
|---|---|---|---|
| `simulator_x3` | X3 | 792 × 528 | no |
| `simulator` | X4 | 800 × 480 | no |

Both profiles are button-driven, so the keyboard below is the whole input
surface. The firmware rotates the buffer, so the window may show a portrait
page from a landscape panel — that is the orientation setting, not a bug.

## Controls

| Key | Action |
|-----|--------|
| ↑ ↓ ← → | Navigate |
| Enter | Confirm |
| Escape | Back |
| L | Toggle display inversion |
| Q | Quit |

On a touch-capable profile a click is a tap and a drag is a swipe. Neither
profile above is touch-capable.

## The Rust screens

Screens written in Rust live in `lib/crosspoint_rs/src/activities/`, mirroring
`src/activities/` on the C++ side. Where one is present, **Settings → System →
Developers** opens it — that screen exercises lists, a slider, a stepper,
sections, scrolling and a modal in one place, which makes it the fastest way to
see a framework change.

The Rust crates are compiled by `pio run` itself, through
`scripts/build_rust.py`. There is no separate cargo step.

See [your-first-rust-screen.md](your-first-rust-screen.md) to write
one, and [rust-ui-framework.md](rust-ui-framework.md) for how the
pieces fit.

## Troubleshooting

**"SDL2 not found"** — install it (above), then
`pio run -e simulator_x3 -t clean && pio run -e simulator_x3`.

**Unknown environment `simulator_x3`** — `platformio.local.ini` is missing or
incomplete. See Setup.

**Window never appears** — check SDL2 is installed; on a remote Linux session
set `DISPLAY`. `pio run -e simulator_x3 -v` shows more.

**Rust build errors** — `./build-and-test.sh` reports them more clearly than the
PlatformIO log. If cargo itself is misbehaving, `rm -rf target` and rebuild;
that costs minutes, because Xtensa has no prebuilt `core`/`alloc`.

**A C++-only rebuild** — `rm -rf .pio/build/simulator_x3/` is enough, and
leaves `target/` alone.

## Known limitations

1. **No SD card** — a local `fs_/` directory stands in
2. **No WiFi** — network features are stubbed, nothing downloads
3. **No hardware buttons** — keyboard only
4. **No OTA** — update paths are compiled out

## Paths

- Binary — `.pio/build/simulator_x3/program`
- Simulated SD card — `fs_/`
- Rust framework — `lib/xpui_rs/`
- Rust screens — `lib/crosspoint_rs/src/activities/`
- C++ side of the bridge — `src/rust_ffi/`, `src/activities/ActivityRs.cpp`

## Resources

- [SDL2 documentation](https://wiki.libsdl.org/)
- [Rust FFI guide](https://doc.rust-lang.org/nomicon/ffi.html)
- [FreeInk SDK](https://freeink.org)

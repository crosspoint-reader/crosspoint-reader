---
name: songbook-simulator
description: Desktop X3 simulator for checking Songbook (or other UI) screens and input without hardware, on macOS. Use when setting up or running the simulator, scripting input and screenshots, or diagnosing simulator build failures.
---

# Songbook simulator

The [crosspoint-simulator](https://github.com/crosspoint-reader/crosspoint-simulator)
compiles the firmware natively and draws the panel in an SDL2 window. It
shows layout, text, input routing, and timer behaviour. It does not model
real heap use, e-ink ghosting, or refresh time, so a simulator pass never
replaces the device checks in the [Songbook rule](../../rules/songbook.md#verification).

## Setup (once per machine)

All of this is local and gitignored. Nothing here is committed.

1. Install SDL2: `brew install sdl2`.
2. Clone the simulator next to this repository:
   `git clone https://github.com/crosspoint-reader/crosspoint-simulator ../crosspoint-simulator`.
   Record its commit; do not edit it locally.
3. Create `platformio.local.ini` (see [local overrides](../../rules/environment.md#platformiolocalini-personal-overrides)).
   Copy `[env:simulator]` and `[env:simulator_x3]` from
   `../crosspoint-simulator/sample-platformio-macos.ini`, then make these changes:
   - `lib_deps`: `simulator=symlink://../crosspoint-simulator`, so the build
     uses the clone you checked.
   - `extra_scripts`: replace
     `pre:.pio/libdeps/$PIOENV/simulator/skip_device_content_protection.py`
     with `pre:../crosspoint-simulator/skip_device_content_protection.py`.
     A `symlink://` dependency installs only a `.pio-link` file, so the
     sample's path does not exist.
4. Put test content in `fs_/`, the simulated SD root:
   `cp -R docs/songbook/sd-card/songbook fs_/`.
5. Use the pinned pioarduino core from
   [getting started](../../../docs/contributing/getting-started.md#first-time-toolchain-setup).
   Two cores that do not match fail with `No module named 'SCons.Tool.FortranCommon'`.

## Build and run

```bash
pio run -e simulator_x3 -t run_simulator
```

The X3 profile is a 792×528 panel. Keys:

| Key | Device button |
| --- | --- |
| ↑ / ↓ | Side buttons |
| ← / → | Left / Right |
| Return | Confirm |
| Esc | Back |
| P | Power |
| S | Sleep |

## Toolchain fallback (old Command Line Tools)

`call to deleted function 'from_chars'` in `CssParser.cpp` means the host
libc++ lacks `std::from_chars` for `float`. The preferred fix is to update
the Command Line Tools to the version for the installed macOS (this needs
the human's admin password). When that is not possible:

1. Run `brew install llvm`.
2. In `[env:simulator]` `extra_scripts`, add
   `pre:.agents/skills/songbook-simulator/simulator_brew_llvm.py` before the
   other pre-scripts. It puts `gcc`/`g++` links to Homebrew clang in the
   gitignored `simulator-llvm.local/` and links Homebrew libc++.
3. Add these lines to `[env:simulator]` `build_flags`. Homebrew libc++ no
   longer includes the C headers that host code relies on implicitly.
   ```
   -include stdlib.h
   -include string.h
   -include stdio.h
   ```

Do not patch `freeink-sdk` or the simulator clone to fix host-toolchain
gaps. Keep each workaround in the local build configuration.

## Scripted checks and screenshots

Script the input and capture screenshots instead of asking the human to click:

```bash
CROSSPOINT_SIM_SD=/path/to/test-sd \
CROSSPOINT_SIM_INPUT_SCRIPT='1500:DOWN;1900:DOWN;2300:ENTER;3700:ENTER;5600:ENTER;7200:ENTER:1200;10500:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS='5000:/path/to/songs.bmp;9600:/path/to/chords.bmp' \
  .pio/build/simulator_x3/program
```

- Times are milliseconds from process start.
- Keys are `UP`, `DOWN`, `LEFT`, `RIGHT`, `ENTER`, `BACK`, `POWER`, `SLEEP`
  and `QUIT`. Add `:<ms>` to hold a key; Songbook holds use 700–1000 ms.
- Put test SD cards and screenshots in the scratchpad, not in `fs_/`, so the
  human's card stays untouched.
- Use a song with a short `{duration}` (for example 30 s) to watch
  auto-scroll steps in one run.
- Convert BMP to PNG before viewing it (for example with Pillow).

On the Home menu, Songbook is the third item when no library slot is shown.
Check the order in a screenshot before you rely on it.

## Self-review

- [ ] The setup lives only in gitignored files, and the simulator clone is unmodified.
- [ ] Screenshots of the changed screens were looked at, not just captured.
- [ ] The report names what the simulator could not verify (heap, ghosting, refresh, orientations not run).

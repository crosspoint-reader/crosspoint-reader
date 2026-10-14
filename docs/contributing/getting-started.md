# Getting Started

This guide helps you build and run CrossPoint locally.

## Prerequisites

- PlatformIO Core (`pio`) or VS Code + PlatformIO IDE
- Python 3.10+ (the build hook `scripts/git_branch.py` needs it)
- `clang-format` 21+ in your `PATH` (CI uses clang-format 21)
- Node.js, then `npm ci` once in the repository root (recommended): it installs terser, which `scripts/build_html.py` uses to minify the embedded web pages' JavaScript. Without it the build still succeeds, prints `terser not found`, and the pages come out larger than a release build's
- USB-C cable
- A supported device for hardware testing: Xteink X3 or X4, Xteink X4 Pro or LilyGo T5S3 (see [Board Support](./board-support.md))

If `./bin/clang-format-fix` fails with either of these errors, install clang-format 21:

- `clang-format: No such file or directory`
- `.clang-format: error: unknown key 'AlignFunctionDeclarations'`

Examples:

```sh
# Debian/Ubuntu (try this first)
sudo apt-get update && sudo apt-get install -y clang-format-21

# If the package is unavailable, add LLVM apt repo and retry
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 21
sudo apt-get update
sudo apt-get install -y clang-format-21

# macOS (Homebrew)
brew install clang-format
```

Then verify:

```sh
clang-format-21 --version
```

The reported major version must be 21 or newer.

## Clone and initialize

```sh
git clone --recursive https://github.com/jpirnay/witchhunt-reader
cd witchhunt-reader
```

The `freeink-sdk` submodule points at our SDK fork, `https://github.com/jpirnay/freeink-sdk` (branch `witchhunt`, see `.gitmodules`). A build against the upstream `Free-Ink/freeink-sdk` main does not compile.

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

Enable the repository-managed Git hooks (required once per clone):

```sh
git config core.hooksPath .githooks
chmod +x .githooks/pre-commit
```

## Build

```sh
pio run                  # default env, Xteink X3 and X4
pio run -e x4pro         # Xteink X4 Pro
pio run -e lilygo_t5s3   # LilyGo T5S3
```

Environments in `platformio.ini`:

| Board | Development | Release | Release candidate |
|---|---|---|---|
| X3 and X4 (ESP32-C3, one binary) | `default` | `gh_release` | `gh_release_rc` |
| X4 Pro (ESP32-S3) | `x4pro` | `x4pro_gh_release` | `x4pro_gh_release_rc` |
| LilyGo T5S3 (ESP32-S3) | `lilygo_t5s3` | `lilygo_gh_release` | `lilygo_gh_release_rc` |

Four more C3 environments are for measurement, not for daily use: `slim` is a release-style build with serial logging compiled out, and `bench` and `bench_font` flash a standalone benchmark that prints timings over serial (no SD card needed). `bench_glyph_store` is a self-test of the TTF glyph store on the real `spiffs` partition; it erases the `.cpfont` flash cache, which the reader copies back from the SD card the next time it needs it.

`default` takes its version (`CROSSPOINT_VERSION`) from the current branch, and the `*_gh_release_rc` envs from the release-candidate tag CI sets in `CROSSPOINT_RC_VERSION`; every other env sets it in `platformio.ini`. Every environment records the display SDK's Git version (`CROSSPOINT_DISPLAY_SDK`). `scripts/git_branch.py` gives those two defines only to the source files that name them, so a branch switch or an SDK bump recompiles about a dozen objects instead of the whole tree. Read them from a `.cpp`, never from a header: a source that includes the header without naming the macro does not get the define. `SystemStatus::firmwareVersion()` and `displaySdkVersion()` in `src/SystemStatus.cpp` show the pattern. During `pio check` and IDE IntelliSense, which compile nothing, the two defines stay global. `CROSSPOINT_GIT_REPOSITORY`, the repository the OTA check asks, is always global.

### Windows: use a short PlatformIO core directory

On Windows, keep PlatformIO's core directory at a short path such as `C:\pio`:

```sh
setx PLATFORMIO_CORE_DIR C:\pio
```

The default (`C:\Users\<name>\.platformio`) makes the build fail with a
`filename or extension is too long` spawn error from the assembler.

Since Arduino-ESP32 3.3.11 the framework contributes ~270 `-I` flags, and GCC
forwards all of them to `as`. Measured on the C3 with the default path, that
command line is **32,771 bytes** against Windows' hard 32,767-character
`CreateProcess` limit -- four bytes over. Every C++ compile dies, and because the
toolchain's `as` is a wrapper that re-spawns the real assembler, it panics
instead of reporting anything useful. A short core directory saves ~18
characters per include path, which is far more than enough headroom.

Linux and macOS are unaffected; their limits are orders of magnitude higher.

## Flash

```sh
pio run --target upload                  # default env
pio run -e x4pro --target upload         # or the env for your board
```

To flash an image that is already built without a rebuild check, use `bin/flash` or `bin\flash.ps1`; see [Testing and Debugging](./testing-debugging.md#flash-and-monitor).

## Host tests

Parsing, layout and cache code is covered by host-side gtest suites in `test/`, built with CMake and run without a device:

```sh
cmake -S test -B build/test
cmake --build build/test
ctest --test-dir build/test --output-on-failure -j
```

Google Test is fetched on the first configure. [Testing and Debugging](./testing-debugging.md) covers running one suite, Windows, and how the suites fit the CI checks.

## First checks before opening a PR

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

## What to read next

- [Architecture Overview](./architecture.md)
- [Development Workflow](./development-workflow.md)
- [Testing and Debugging](./testing-debugging.md)

# Getting Started

This guide helps you build and run CrossPoint locally.

## Prerequisites

- PlatformIO Core (`pio`) or VS Code + PlatformIO IDE
- Python 3.8+
- `clang-format` 21+ in your `PATH` (CI uses clang-format 21)
- USB-C cable
- Xteink X4 device for hardware testing

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
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader
```

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
pio run
```

> On macOS, Homebrew's `pio` may lack the `littlefs` module the espressif32
> builder needs. If a build fails with `ModuleNotFoundError: No module named
> 'littlefs'`, use `~/.platformio/penv/bin/pio` instead — PlatformIO's own
> virtualenv has it. `./build-and-test.sh` picks that one automatically when it
> is present.

## Flash

```sh
pio run --target upload
```

## First checks before opening a PR

```sh
./build-and-test.sh all
```

One command for every gate and every environment CI builds — including the Rust
workspace, which `pio run` on its own does not cover. By hand:

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e default
```

CI builds more environments than `default`; `.github/workflows/ci.yml` is the
authoritative list, and `./build-and-test.sh all` builds them in one invocation.

## What to read next

- [Architecture Overview](./architecture.md)
- [Development Workflow](./development-workflow.md)
- [Testing and Debugging](./testing-debugging.md)

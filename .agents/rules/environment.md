## Development Environment Awareness

### Platform Detection

Run `uname -s` once at session start and select tools/commands for the host:
`MINGW64_NT-*` = Windows Git Bash, `Linux` = Linux, `Darwin` = macOS.

### Platform-Specific Behaviors

- Windows Git Bash: Unix commands; Windows `C:\` paths, `/` in bash;
  limited glob support, so use `find` + `xargs`.
- Linux/WSL: full bash, Unix paths, native glob support.

For formatting on every host, use `./bin/clang-format-fix -g` exclusively.
Never invoke or probe `clang-format` directly.

### Build in an existing environment

Use the installed toolchain and existing packages. Run `pio run -e <environment>`
directly (`pio run` selects `default`). Preserve local configuration and build
caches. Install, upgrade, re-pin, or re-download tools only when the human
requests setup or a concrete build failure establishes a missing or incompatible
dependency. Diagnose that failure first and repair only the affected component.

The [getting-started setup commands](../../docs/contributing/getting-started.md#first-time-toolchain-setup)
are for first-time setup or an identified environment problem, not routine builds.

---


## Local Development Configuration

### platformio.local.ini (Personal Overrides)

**Purpose**: Personal development settings that should NEVER be committed.

**Use Cases**:

- Serial port configuration (varies by machine)
- Debug flags for specific testing
- Local build optimizations
- Developer-specific paths

**Example** `platformio.local.ini`:

```ini
# platformio.local.ini (gitignored)
[env:default]
upload_port = COM7              # Windows: COMx, Linux: /dev/ttyUSBx
monitor_port = COM7

build_flags =
  ${base.build_flags}
  -DFREEINK_DEVICE_X4=1
  -DFREEINK_DEVICE_X3=1
  -DENABLE_SERIAL_LOG
  -DCROSSPOINT_WAIT_FOR_USB_SERIAL
  -DLOG_LEVEL=2
  -DMY_DEBUG_FLAG=1             # Personal debug flags
  -DTEST_FEATURE_ENABLED=1
```

**Configuration Hierarchy**:

1. `platformio.ini` - **Committed**, shared project settings
2. `platformio.local.ini` - **Gitignored**, personal overrides
3. Local file extends/overrides base config

**Rules**:

- **NEVER commit** `platformio.local.ini`
- **NEVER put** personal info (serial ports, credentials) in main `platformio.ini`
- A local `build_flags` value replaces the selected environment's list.
  Include `${base.build_flags}` plus that environment's device, capability,
  logging, and version flags before adding personal flags. The example above
  preserves the current `default` flags; check `platformio.ini` for other boards.

Select an existing board profile from `platformio.ini` before creating a local
one; non-Xteink examples are in
[docs/contributing/touch-and-ui.md](../../docs/contributing/touch-and-ui.md).
Consult [getting started](../../docs/contributing/getting-started.md) when setup
is needed. Git branch/SHA version flags apply only to sources that use
`CROSSPOINT_VERSION`; changing branches does not require a clean rebuild.

If an interrupted custom-core rebuild leaves duplicate `app_main` definitions,
use the targeted scaffold cleanup documented beside `[firmware_tuned]` in
`platformio.ini`. Preserve personal overrides and unrelated ignored files;
`git clean -fdX` would delete `platformio.local.ini` too.

---

## Development Environment Awareness

### Platform Detection

Use the session's `uname -s`: `MINGW64_NT-*` is Windows Git Bash,
`Linux` is Linux/WSL, and `Darwin` is macOS. Select host-compatible commands.
Formatting follows the [wrapper rule](testing-debugging.md#code-quality).

### Build in an existing environment

Use installed tools/packages and preserve local configuration/build caches.
Run the selected build directly. Install, upgrade, re-pin, or re-download tools
only for requested setup or a concrete missing/incompatible dependency.
Diagnose that failure first and repair only the affected component.

[Getting-started setup](../../docs/contributing/getting-started.md#first-time-toolchain-setup)
is for first-time setup or an identified environment failure, not routine builds.
Build output follows [compact logging](testing-debugging.md#compact-build-output).

## Local Development Configuration

### platformio.local.ini (Personal Overrides)

`platformio.ini` is shared; ignored `platformio.local.ini` holds personal
ports/debug options. Never stage local overrides or put personal ports/credentials
in shared configuration.

A local `build_flags` value replaces the selected environment's list.
Preserve `${base.build_flags}` plus that environment's device, capability,
logging, and version flags before adding personal flags. Inspect
`platformio.ini`; select an existing board profile before creating a local one.

Git branch/SHA version flags apply only where `CROSSPOINT_VERSION` is used;
changing branches alone does not require a clean build. For duplicate
`app_main` after an interrupted custom-core rebuild, use the targeted scaffold
cleanup beside `[firmware_tuned]`. Preserve overrides and unrelated ignored work;
`git clean -fdX` would delete them.

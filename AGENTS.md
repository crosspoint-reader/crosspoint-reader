# Reader agent guide

Personal fork of CrossPoint Reader: e-reader firmware for ESP32 e-ink devices.
Goal: a feature-rich but minimal OS for reading and learning. See `SCOPE.md`.

## Targets

| Env | Device | MCU / memory |
| --- | --- | --- |
| `default` (+ `slim`) | Xteink X3 / X4 | ESP32-C3, ~380 KB RAM, no PSRAM |
| `x4pro`, `x4c` | Xteink X4 Pro / X4 Classic | ESP32-S3, 8 MB PSRAM |
| `eego_a4` | EEGO Reader A4 | ESP32-S3, 8 MB PSRAM |
| `sticky` | Seeed reTerminal Sticky | ESP32-S3, 8 MB PSRAM |

The X3/X4 (ESP32-C3) is the limit: every feature must fit there, or be gated
so it costs nothing on that build. Avoid large or long-lived heap allocations
and prefer streaming from the SD card over loading whole files.

Hardware drivers and board profiles live in the `freeink-sdk/` submodule
(`git submodule update --init` if it is empty). Keep device-specific code
behind the HAL/SDK, not in reader code.

## Commands

- Build: `pio run -e <env>` (`pio run` builds `default`).
- Host tests: `pio run -t unit-tests`, or CMake/CTest in `test/`.
- Format: `./bin/clang-format-fix -g` (do not run raw `clang-format`).
- Translations: edit `lib/I18n/translations/*.yaml`, then
  `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/`.
- Web UI: edit `src/network/html/`; the `*.generated.h` files are built automatically.

## How to work

- Keep it simple. Reuse existing activities, list/UI components and stores
  before adding new ones.
- Verification is proportionate: build the affected env(s) and run the
  relevant host tests. No multi-agent reviews unless asked.
- Hardware testing is done by the owner.
- Commit only when asked; push only when asked.
- If a cache/file format changes, bump its version (see
  `.agents/rules/generated-files-cache.md`).

## Reference (read only when relevant)

- `.agents/rules/hardware-resources.md`: memory and flash limits
- `.agents/rules/coding-standards.md`: naming, comments, pitfalls
- `.agents/rules/ui-activities.md`: activities, input, lists, workers, fonts
- `.agents/rules/architecture-hal.md`: HAL, build flags, services
- `.agents/rules/generated-files-cache.md`: generated files, cache versioning

---
name: platform-targets
description: "Device and PlatformIO-env facts for CrossPoint. Use when the task mentions a board, env, FREEINK_DEVICE_*, PSRAM / BOARD_HAS_PSRAM, panel size, framebuffer, controller, touch, frontlight, uiScale, bezel insets, or 'does this work on this board'; when adding a board or compile-time cap; or when AGENTS.md defers a hardware number here."
---

# Platform Targets

`AGENTS.md` states rules that hold for every device this tree compiles. This
skill holds the numbers. One resource file per **device** on the compile set,
not a fixed board list and not one file per PIO env name. Heap, HAL, and scope
skills stay procedures; they point here for facts.

Repo language: **env** is a PlatformIO binary (`pio run -e default`). **device**
is a `FREEINK_DEVICE_*` flag. **board profile** is the SDK `BoardProfile` in
`freeink-sdk`. Do not treat those three as the same thing.

## Procedure

1. **Can build.** Parse committed `platformio.ini` for `[env:…]` and that env's
   `-DFREEINK_DEVICE_*`, `-DBOARD_HAS_PSRAM`, `-DUSE_BLOCK_DEVICE_INTERFACE`.
   Treat `*-gh_release*` as aliases of the same env class. Ignore
   `platformio.local.ini` as a contract (see below).
2. **Should build.** Intersect with CI `pio run -e` jobs in
   `.github/workflows/ci.yml`. If INI and CI disagree, say so; do not invent a
   third list. `pio check` is not a device gate (it follows the default env's
   include graph).
3. **Load resources.** For each `FREEINK_DEVICE_*` on that compile set, read
   `resources/<device>.md` (slug = flag suffix, lowercased). Do not glob
   `resources/` and treat every file as live — only those devices. One env may
   set several flags (one shared binary); load every matching file.
4. **All devices this tree builds.** Union those resources. A change that
   cannot satisfy every present device without a new activity or a PSRAM-only
   heap is not a CrossPoint-wide change.
5. **Conflict.** If a number here disagrees with `AGENTS.md`, this skill and
   the INI win. Fix the guide; do not "correct" firmware to match stale prose.

Hardware truth is in-tree
`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h`.
`https://freeink.org/llms.txt` is a website index, not `BoardConfig`.

## `platformio.local.ini`

Gitignored, desk-only, merged via `extra_configs`. It is **not** the compile
contract. Do not silently treat a local env as first-class.

If you see an `[env:…]` or `FREEINK_DEVICE_*` there that is missing from the
committed compile set, **ask** whether to research the definition
(`BoardConfig.h`, local flags) and add a resource. If they say no, stop. They
can ask later.

## Refreshing resources from sources

Field meanings live in [SCHEMA.md](SCHEMA.md). This skill may edit its own
`resources/`. Re-read the sources below whenever this skill loads and a source
is newer than the resource, or when the task changes an env, a device flag, CI
`pio run -e`, the `freeink-sdk` submodule, or a `BoardProfile`.

**Compile set** (which files may exist and be applied):

| Ask | Source |
| --- | --- |
| Which `[env:…]` exist? | committed `platformio.ini` |
| Which device flags does each env set? | that env's `-DFREEINK_DEVICE_*` |
| Which envs CI builds? | `.github/workflows/ci.yml` `pio run -e` |
| Shared-binary aliases? | same flags on `*-gh_release*`, `*-gh_release_rc`, `slim` siblings |

A `resources/<device>.md` is in play only if that flag is on the compile set.
If a flag is added: create the file from SCHEMA plus the field map. If a flag
leaves the set: delete its resource. Extra files in `resources/` are leftover;
do not apply them.

**Per-device fields** (where each number comes from):

| Field | Source |
| --- | --- |
| `device` / `device_flag` | `FREEINK_DEVICE_*` in the INI (slug = flag suffix, lowercased) |
| `sdk_profile` / `sdk_header` | `constexpr BoardProfile` in `BoardConfig.h` |
| `shared_binary_envs` | every committed env that sets this flag |
| `board_package` | `board =` on those envs |
| `mcu_family` | that PlatformIO board / `board_build.mcu` |
| `psram_in_ini` | `-DBOARD_HAS_PSRAM` on those envs (S3 silicon ≠ this) |
| `psram_on_silicon` | `BoardProfile` comments / chip (e.g. S3 n16r8) |
| `fb_in_psram` | `FREEINK_FB_PSRAM` default for this device in `BoardConfig.h` |
| `sdmmc` | `BoardProfile.sdmmc.busWidth != 0` |
| `block_device_interface` | `-DUSE_BLOCK_DEVICE_INTERFACE` on those envs |
| `width` / `height` | `BoardProfile.displayWidth` / `displayHeight` |
| `fb_bytes` | `width/8 * height` for **this** profile. Compiled max is `BoardConfig::MAX_FRAMEBUFFER_BYTES` (union of flags on that env). |
| `controllers` | `DisplayController` plus sibling profiles selected at runtime for that device |
| `grayscale` | driver / profile comments in `BoardConfig.h` |
| `viewable_insets` | `BoardProfile.viewableInsets` if set; else the `ViewableInsets` member defaults (9/3/3/3, not zeros) |
| `touch` / `frontlight` / `ui_scale` | `touch`, `frontlight`, `uiScale` on the profile |
| `ppi_note` | SDK comments only; do not invent dpi |
| `caps` | `FREEINK_CAP_*` macros in `BoardConfig.h` that include this device |

Do not take panel size, PSRAM, or caps from `AGENTS.md`, `llms.txt`, or a
resource that is not on the compile set.

## Self-review

- [ ] Used committed `platformio.ini` ∩ CI, not `platformio.local.ini`, as the
      compile set.
- [ ] Loaded one resource per device flag on that set (several flags on one
      env → several files).
- [ ] Did not apply a `resources/` file whose flag is missing from the set.
- [ ] Refreshed any stale resource from `BoardConfig.h` / INI / CI using the
      field map, or created/deleted a file when the set changed.
- [ ] Asked before promoting a local-only env; respected a no.
- [ ] Hardware numbers came from the compile-set resources, not from this file's
      prose.
- [ ] PSRAM / `MALLOC_CAP_SPIRAM` only if the env being built sets
      `BOARD_HAS_PSRAM`; C3 `default` still compiles without that allocation.

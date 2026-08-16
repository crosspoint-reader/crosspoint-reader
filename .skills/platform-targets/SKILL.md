---
name: platform-targets
description: "Device and PlatformIO-env facts for CrossPoint. Use when the task mentions a board, env, FREEINK_DEVICE_*, PSRAM, panel, framebuffer, controller, touch, frontlight, uiScale, bezel insets, or other device hardware capabilities; when adding, correcting, or removing a device or env; when changing this skill's resource schema; or when AGENTS.md defers a hardware number here."
---

# Platform Targets

`AGENTS.md` states rules that hold for every device this tree **compiles**.
This skill holds per-device numbers. One resource file per **device**, not per
PIO env name, and not a closed board list. Heap, HAL, and scope skills stay
procedures; they point here for facts.

Repo language: **env** is a PlatformIO binary (`pio run -e default`). **device**
is a `FREEINK_DEVICE_*` flag. **board profile** is the SDK `BoardProfile` in
`freeink-sdk`. Do not treat those three as the same thing.

If the next step is a judgment call **about something you are already
touching** (delete that resource, invent INI/CI, promote a local env, change
the schema), **ask**. Do not guess. Do not ask about a resource or env you
are not editing.

## When to open a resource

Open `resources/<device>.md` when the task names that device, env, or flag, or
when that flag is on the compile set. Do not glob `resources/` and treat every
file as a must-build constraint. Files you have not opened stay out of the
union.

Keep **every** committed file in `resources/` current when you refresh from
sources or change the schema — including files whose flags are not on this
checkout's compile set.

## Compile set

Committed `platformio.ini` ∩ CI. This is what firmware on **this checkout**
must still build. Enumerate every flag on that set; omitting one is a lie.
It does **not** limit which resource files may exist or be refreshed.

1. **Can build.** Parse committed `platformio.ini` for `[env:…]` and that env's
   `-DFREEINK_DEVICE_*`, `-DBOARD_HAS_PSRAM`, `-DUSE_BLOCK_DEVICE_INTERFACE`.
   Release and slim siblings (`gh_release`, `gh_release_rc`, `sticky-gh_release`,
   `slim`, and the same family under other prefixes) are the same env class.
   Match by name family, not a glob. The list is not exhaustive.
2. **Should build.** Collect every `-e` argument and every CI `matrix.env` from
   `pio run` lines in `.github/workflows/ci.yml`. If INI and CI disagree, say
   so; do not invent a third list. `pio check` is not a device gate.
3. **Must-build union.** For each flag on that set, read the matching
   resource. One env may set several flags; load every matching file. A
   CrossPoint-wide firmware change must satisfy that union. Other resource
   files do not widen it.
4. **Conflict.** If a number here disagrees with `AGENTS.md`, this skill and
   the INI win. Fix the guide.

`platformio.local.ini` is desk-only. A local-only env is a question only if
the task is about that env (research / add a resource / add a committed env).
Respect a no.

Hardware truth for this checkout is in-tree
`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h`.
That header lists more devices than this tree compiles; do not treat every
`BoardProfile` as a CrossPoint target.

`https://freeink.org/llms.txt` is a website index — useful for general
hints, not that header. Before you change the SDK or propose new device
support, you may suggest reviewing that index and the latest branch of
upstream `freeink-sdk` (https://github.com/Free-Ink/freeink-sdk) so
published notes and newer profiles are not missed. Do not fill resource
fields from either. Neither overrides the in-tree header on this checkout.

## Interactions

Match the task. Do not assume one of these is the only way in.

1. **Add a committed env / CI job.** Create or refresh that device's resource
   from the field map. This skill owns the resource; it does not invent INI
   or CI unless the task includes those edits.
2. **Correct facts.** A resource disagrees with `BoardConfig.h`, the INI, or
   CI. Re-read the sources, patch the file, say what changed. Do not "fix"
   firmware to match stale prose.
3. **Remove an env or a resource.** **Ask** whether to delete the resource
   only if that file or env is in the change. Do not propose deleting a
   sibling file you are not touching.
4. **Change the schema.** Edit [SCHEMA.md](SCHEMA.md) — fields **and** the
   per-item comments that list options — then every file in `resources/` so
   none omit the new field or still name a deleted option. Do not drive-by
   schema-edit.
5. **Experiment / local build cleanup.** Follow the compile set on this
   checkout. Ask about deleting a resource only if that file is part of the
   experiment.

## Refreshing resources from sources

Field meanings live in [SCHEMA.md](SCHEMA.md). This skill may edit its own
`resources/` and `SCHEMA.md`.

If **you** change a source this skill reads, update the dependents in the
same change. Do not leave SCHEMA comments or resource files describing the
old integration. Sources: `BoardConfig.h` (profiles, enums, `FREEINK_CAP_*`,
`FREEINK_FB_PSRAM`, `ViewableInsets`), committed `platformio.ini`,
`.github/workflows/ci.yml`, and [SCHEMA.md](SCHEMA.md) itself.

When any of those move, refresh **all** committed resource files, not only
the ones on this compile set, and re-vet SCHEMA comments against the
integrations and against every file in `resources/` (see SCHEMA.md).

| Ask | Source |
| --- | --- |
| Which `[env:…]` exist here? | committed `platformio.ini` |
| Which device flags does each env set? | that env's `-DFREEINK_DEVICE_*` |
| Which envs CI builds? | every `-e` / `matrix.env` on `pio run` in `.github/workflows/ci.yml` |
| Shared-binary aliases? | same flags on release/slim siblings of that env (name family, not a glob) |

| Field | Source |
| --- | --- |
| `device` / `device_flag` | `FREEINK_DEVICE_*` (INI if present, else BoardConfig / the requested flag) |
| `sdk_profile` / `sdk_header` | `constexpr BoardProfile` in `BoardConfig.h` |
| `shared_binary_envs` | every committed env on **this** tree that sets this flag (`[]` if none) |
| `board_package` | `board =` on those envs, or the SDK sample / profile comments if none |
| `mcu_family` | that PlatformIO board / `board_build.mcu` |
| `psram_in_ini` | `-DBOARD_HAS_PSRAM` on those envs (false if no such env here) |
| `psram_on_silicon` | `BoardProfile` comments / chip |
| `fb_in_psram` | `FREEINK_FB_PSRAM` default for this device in `BoardConfig.h` |
| `sdmmc` | `BoardProfile.sdmmc.busWidth != 0` |
| `block_device_interface` | `-DUSE_BLOCK_DEVICE_INTERFACE` on those envs (false if none) |
| `width` / `height` | `BoardProfile.displayWidth` / `displayHeight` |
| `fb_bytes` | `width/8 * height` for **this** profile. Compiled max is `BoardConfig::MAX_FRAMEBUFFER_BYTES` for the flags on that env. |
| `controllers` | `DisplayController` plus sibling profiles selected at runtime for that device |
| `grayscale` | driver / profile comments in `BoardConfig.h` |
| `viewable_insets` | `BoardProfile.viewableInsets` if set; else the `ViewableInsets` member defaults (9/3/3/3, not zeros) |
| `touch` / `frontlight` / `ui_scale` | `touch`, `frontlight`, `uiScale` on the profile |
| `ppi_note` | SDK comments only; do not invent dpi |
| `caps` | `FREEINK_CAP_*` macros in `BoardConfig.h` that include this device |

Do not take panel size, PSRAM, or caps from `AGENTS.md`, `llms.txt`, or
an upstream SDK checkout that is not this submodule. Those may inform an
SDK or new-support review; they do not override in-tree `BoardConfig.h`.

## Self-review

- [ ] Opened only the resources the task or the compile-set flags called for.
- [ ] Compile-set enumeration listed every INI ∩ CI flag (every `-e` and
      `matrix.env`), or said INI and CI disagree.
- [ ] Must-build union used only those flags, not every file in `resources/`.
- [ ] If a relied-on source moved, refreshed every committed resource **and**
      SCHEMA comments; no leftover option or missing new option.
- [ ] Did not ask about a file or env I was not touching.
- [ ] Asked before deleting a resource that *is* in the change; respected a no.
- [ ] PSRAM / `MALLOC_CAP_SPIRAM` only if the env being built sets
      `BOARD_HAS_PSRAM`; the tightest compiled DRAM target still builds
      without that allocation.

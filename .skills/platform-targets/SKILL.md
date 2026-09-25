---
name: platform-targets
description: "Device and PlatformIO-env facts for CrossPoint. Use when the task mentions a board, env, FREEINK_DEVICE_*, PSRAM, panel, framebuffer, controller, touch, multitouch, home key, frontlight, USB detect, shared GPIO pads, uiScale, bezel insets, or other device hardware capabilities; when adding, correcting, or removing a device or env; when changing this skill's resource schema; when optionally pre-warming the PlatformIO .pio cache; or when AGENTS.md defers a hardware number here."
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

## Warm the PlatformIO cache

Not required. Skip this unless you want a warm `.pio/`. If you do, this
is a consistent way to fill it. Compiling only the env the task needs is
enough.

The expensive part is the first compile of ESP-IDF/Arduino, the SDK, and
shared CrossPoint TUs. After that, switching devices is mostly relinking
plus the files that differ per `-DFREEINK_DEVICE_*`.

Warm **sequentially**. Do not run two `pio run -e` at once — they share
`.pio/`. Firmware compile only; do not flash or attach hardware. Upload
and monitor are separate.

```bash
pio run -e default
pio run -e x4pro
pio run -e sticky
pio run -e papermono
```

`default` first: C3, X3+X4 in one binary, tightest DRAM. `x4pro` pulls
the S3 toolchain and the three X4 Pro drivers (SSD1677 / UC8179 / UC8279).
`sticky` and `papermono` fill the rest of the CI compile set. These are
the representative envs, not every release/slim sibling. If INI ∩ CI
grows or shrinks, update this list from the compile set — do not invent
a third order.

`pio run -t clean` throws that warmth away. Incremental rebuilds after
checking out a close branch stay fast if you leave `.pio/` in place.

Optional host-side warmth only if you want it: leave the PlatformIO penv
as-is (`~/.platformio/`). If you use `compile_commands.json` / clangd,
generate that once after a successful `default` build — do not regenerate
per env.

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
`FREEINK_FB_PSRAM`, `ViewableInsets`, `TouchConfig.hasHomeKey`),
`InputManager::supportsMultiTouch` (for `multitouch`),
`HalGPIO::isUsbConnected` (for `usb_detect`), committed `platformio.ini`,
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
| `multitouch` | `InputManager::supportsMultiTouch` — true only when `BoardProfile.touch.controller` is `TouchController::Gt911`. Other controllers stay single-contact. |
| `has_home_key` | `BoardConfig::hasHomeKey` / `TouchConfig.hasHomeKey` |
| `usb_detect` | `HalGPIO::isUsbConnected` — `bq27220` when `deviceIsX3()`, else `gpioN` from `BoardProfile.usbDetect` if `>= 0`, else `none`. Do not copy a raw `usbDetect >= 0` as a VBUS pin. |
| `shared_pads` | This device's role for pads that collide across the compile set (today GPIO13 / GPIO20) or whose firmware use differs from the raw profile field. Not a full pinout. |
| `ppi_note` | SDK comments only; do not invent dpi |
| `caps` | `FREEINK_CAP_*` macros in `BoardConfig.h` that include this device |

Do not take panel size, PSRAM, or caps from `AGENTS.md`, `llms.txt`, or
an upstream SDK checkout that is not this submodule. Those may inform an
SDK or new-support review; they do not override in-tree `BoardConfig.h`.

A vendor product page is not a released unit. Cite that page in the
resource body. If a schematic PDF is a sublink of the page, cite it
under the page — that is one vendor source, generally authoritative for
the published pin map / sheet. A GitHub attachment of the same file is a
rehost, not a second origin. YAML follows the table above (current
checkout firmware). Until a released unit is measured, list divergences
and the first source of each firmware choice — do not rewrite firmware
to match the page.

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
- [ ] Did not run two `pio run -e` at once. Did not `pio run -t clean` a
      warm `.pio` unless asked.

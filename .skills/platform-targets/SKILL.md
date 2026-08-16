---
name: platform-targets
description: "Device and PlatformIO-env facts for CrossPoint. Use when the task mentions a board, env, FREEINK_DEVICE_*, PSRAM / BOARD_HAS_PSRAM, panel size, framebuffer, controller, touch, frontlight, uiScale, or bezel insets; when adding, correcting, removing, or pre-emptively documenting a device; when changing this skill's resource schema; or when AGENTS.md defers a hardware number here."
---

# Platform Targets

`AGENTS.md` states rules that hold for every device this tree **compiles**.
This skill holds per-device numbers. One resource file per **device**, not per
PIO env name, and not a closed board list. Heap, HAL, and scope skills stay
procedures; they point here for facts.

Repo language: **env** is a PlatformIO binary (`pio run -e default`). **device**
is a `FREEINK_DEVICE_*` flag. **board profile** is the SDK `BoardProfile` in
`freeink-sdk`. Do not treat those three as the same thing.

If the next step is a judgment call (delete a file, invent INI/CI, promote a
local env, change the schema, treat a file as leftover), **ask**. Do not guess.

Two lists, both factual — do not collapse them:

- **Compile set** — committed `platformio.ini` ∩ CI `pio run -e`. This is what
  the firmware must still build. Enumerate every flag on that set; omitting one
  is a lie.
- **Skill resources** — every `resources/<device>.md`. This may be a **superset**
  of the compile set (upcoming agent support with no INI/CI change yet). A
  resource with `status: upcoming` is not a compile target. A compile-set flag
  with no file, or a file still marked `upcoming`, is a judgment call: **ask**.
  If that work is landing on `develop`, `master`, or the remote's default
  branch, ask specifically whether to flip `upcoming` → `compiled`.

## Interactions

Match the task. Do not assume one of these is the only way in.

1. **Add firmware support.** The task is adding a committed env and CI job.
   Create or refresh `resources/<device>.md` from the field map, set
   `status: compiled`, and keep SCHEMA and the new INI/CI in lockstep. This
   skill owns the resource; it does not invent INI or CI unless the task
   includes those edits.
2. **Correct facts.** A resource disagrees with `BoardConfig.h`, the INI, or
   CI, or a reviewer found a wrong number. Re-read the sources, patch the
   file, say what changed. Do not "fix" firmware to match stale prose.
3. **Remove firmware support.** The task drops a flag from the committed
   compile set. **Ask** whether to delete the resource or keep it as
   `status: upcoming`. A file that is not on the compile set is not
   automatically leftover and is not automatically deleted.
4. **Change the schema.** Allowed when the current fields cannot state a fact
   (new source, new BoardProfile member, a field that was wrong). Edit
   [SCHEMA.md](SCHEMA.md), then every resource (compiled and upcoming) so
   none omit the new field. Do not drive-by schema-edit during an unrelated
   change.
5. **Pre-emptive agent support.** The task wants a resource for a device that
   is **not** on the compile set yet, and nothing else (no INI, no CI, no
   firmware). Create `resources/<device>.md` from `BoardConfig.h` (and SDK
   sample envs if the CrossPoint INI has no stanza), set `status: upcoming`,
   `shared_binary_envs: []`. Do not apply it as something this tree builds.
   Do not add INI/CI unless they ask.

`platformio.local.ini` is still desk-only. A local-only env is a question
(research / add upcoming resource / add real support), not a silent compile-set
promotion. Respect a no.

## Procedure (compile-set work)

1. **Can build.** Parse committed `platformio.ini` for `[env:…]` and that env's
   `-DFREEINK_DEVICE_*`, `-DBOARD_HAS_PSRAM`, `-DUSE_BLOCK_DEVICE_INTERFACE`.
   Treat `*-gh_release*` as aliases of the same env class.
2. **Should build.** Intersect with CI `pio run -e` in
   `.github/workflows/ci.yml`. If INI and CI disagree, say so; do not invent a
   third list. `pio check` is not a device gate.
3. **Load compiled resources.** For each flag on that set, read
   `resources/<device>.md` (slug = flag suffix, lowercased). One env may set
   several flags; load every matching **compiled** file. If the file is missing
   or marked `upcoming` while the flag is on the compile set, **ask** (create,
   flip status, or leave it) unless the task already said which. If the change
   is landing on `develop`, `master`, or the remote's default branch, ask
   about flipping `upcoming` → `compiled`.
4. **Union.** A CrossPoint-wide firmware change must satisfy every **compiled**
   device. Upcoming resources inform design; they do not widen the must-build
   set.
5. **Conflict.** If a number here disagrees with `AGENTS.md`, this skill and
   the INI win. Fix the guide.

Hardware truth is in-tree
`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h`.
`https://freeink.org/llms.txt` is a website index, not `BoardConfig`.

## Refreshing resources from sources

Field meanings live in [SCHEMA.md](SCHEMA.md). This skill may edit its own
`resources/` and `SCHEMA.md`. Re-read sources when this skill loads and a
source is newer than the resource, or when the task is one of the interactions
above.

**Compile set**

| Ask | Source |
| --- | --- |
| Which `[env:…]` exist? | committed `platformio.ini` |
| Which device flags does each env set? | that env's `-DFREEINK_DEVICE_*` |
| Which envs CI builds? | `.github/workflows/ci.yml` `pio run -e` |
| Shared-binary aliases? | same flags on `*-gh_release*`, `*-gh_release_rc`, `slim` siblings |

**Per-device fields**

| Field | Source |
| --- | --- |
| `status` | `compiled` if the flag is on the compile set; `upcoming` if the file exists only for agent support |
| `device` / `device_flag` | `FREEINK_DEVICE_*` in the INI, or the BoardConfig / requested flag when upcoming |
| `sdk_profile` / `sdk_header` | `constexpr BoardProfile` in `BoardConfig.h` |
| `shared_binary_envs` | every committed env that sets this flag (`[]` if upcoming) |
| `board_package` | `board =` on those envs, or the SDK sample / profile comments if upcoming |
| `mcu_family` | that PlatformIO board / `board_build.mcu` |
| `psram_in_ini` | `-DBOARD_HAS_PSRAM` on those envs (S3 silicon ≠ this; false if no env yet) |
| `psram_on_silicon` | `BoardProfile` comments / chip |
| `fb_in_psram` | `FREEINK_FB_PSRAM` default for this device in `BoardConfig.h` |
| `sdmmc` | `BoardProfile.sdmmc.busWidth != 0` |
| `block_device_interface` | `-DUSE_BLOCK_DEVICE_INTERFACE` on those envs (false if no env yet) |
| `width` / `height` | `BoardProfile.displayWidth` / `displayHeight` |
| `fb_bytes` | `width/8 * height` for **this** profile. Compiled max is `BoardConfig::MAX_FRAMEBUFFER_BYTES` for the flags on that env. |
| `controllers` | `DisplayController` plus sibling profiles selected at runtime for that device |
| `grayscale` | driver / profile comments in `BoardConfig.h` |
| `viewable_insets` | `BoardProfile.viewableInsets` if set; else the `ViewableInsets` member defaults (9/3/3/3, not zeros) |
| `touch` / `frontlight` / `ui_scale` | `touch`, `frontlight`, `uiScale` on the profile |
| `ppi_note` | SDK comments only; do not invent dpi |
| `caps` | `FREEINK_CAP_*` macros in `BoardConfig.h` that include this device |

Do not take panel size, PSRAM, or caps from `AGENTS.md` or `llms.txt`. Do not
treat an `upcoming` file as compiled.

## Self-review

- [ ] Named which interaction this was (add / correct / remove / schema /
      pre-emptive / compile-set lookup).
- [ ] Compile-set enumeration listed every INI ∩ CI flag, or said INI and CI
      disagree.
- [ ] Applied only `status: compiled` resources as must-build constraints.
- [ ] Did not delete a resource, or treat one as leftover, without asking.
- [ ] Asked when a compiled flag had no file or an `upcoming` file; asked
      about flipping if landing on `develop`, `master`, or the default branch.
- [ ] Schema edits updated SCHEMA.md and every resource, and were the task.
- [ ] Asked on judgment calls (local env, leftover file, status mismatch,
      schema, INI/CI); respected a no.
- [ ] PSRAM / `MALLOC_CAP_SPIRAM` only if the env being built sets
      `BOARD_HAS_PSRAM`; the tightest compiled DRAM target still builds
      without that allocation.

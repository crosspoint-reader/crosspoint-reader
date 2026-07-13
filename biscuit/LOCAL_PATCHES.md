# biscuit/LOCAL_PATCHES.md

**Purpose.** `biscuit/` is vendored from upstream `yattsu/biscuit` (branch `master`) via
`git subtree --squash`. Every edit below lives *under this prefix*, so each
`git subtree pull` (run `tools/sync-biscuit.sh`) can conflict on the **modified** files.
This file is the crib sheet that makes resolving those conflicts mechanical.

**Sentinel convention.** Every in-place edit carries a grep-able marker:
- in C/C++: `// [BOOTSWITCH-PATCH] Pn`
- in `.ini`: `; [BOOTSWITCH-PATCH] Pn`

(The i18n yaml block carries NO marker: Biscuit's `gen_i18n.py` hard-fails on comment
lines in translation files. Locate those keys with `grep -n STR_SWITCH_OS
biscuit/lib/I18n/translations/english.yaml` instead.)

After any sync, run:

```bash
grep -rn --exclude=LOCAL_PATCHES.md "BOOTSWITCH-PATCH" biscuit/
```

Expected: **14 marker lines** across 7 files (P1–P3 ×1 each in platformio.ini; P4 ×2
— the two source-file headers; P5 ×4; P6 ×2; P7 ×3). A missing marker means the
merge dropped that patch — re-apply from this file.

**Golden rule:** keep biscuit-side edits minimal. All swap *logic* lives in the parent
tree (`../lib/BootSwitch/`); these patches are just wiring.

**Useful context:** Biscuit is itself a CrossPoint fork (same Activity/ActivityManager,
ConfirmationActivity, UITheme, i18n and lib/Logging stack), which is why the patches
mirror the CrossPoint-side implementation almost verbatim.

---

## P1 — `biscuit/platformio.ini` : make the nested build see BootSwitch
**Kind:** modified · **Sync risk:** HIGH (shared file, 3 patches) · line ~65

```ini
; [BOOTSWITCH-PATCH] P1: shared dual-OS swap module from the parent repo root
  BootSwitch=symlink://../lib/BootSwitch
```

Prepended to the existing `lib_deps` list. *Deviation from the original plan:* a
`symlink://` lib_deps entry (the repo's own idiom) instead of `lib_extra_dirs =
${PROJECT_DIR}/../lib`, which would expose all ~24 CrossPoint libs to Biscuit's LDF and
invite name collisions (both trees have `Logging`, `I18n`, `hal`, …).

`BOOTSWITCH_USE_ESP_LOG` is **not** set: Biscuit ships the same `lib/Logging` as
CrossPoint, so BootSwitch's default `<Logging.h>` path compiles here.

This install path requires `lib/BootSwitch/library.json` in the parent repo: anything
pulled in via `lib_deps` goes through PlatformIO's Library Manager, which refuses
packages without a manifest (`MissingPackageManifestError`) — do not remove it as
"unused" just because the other internal libs have none.

**On conflict:** re-add the line inside `lib_deps`; keep upstream's entries.

## P2 — `biscuit/platformio.ini` : use the canonical shared partition table
**Kind:** modified (deliberate override) · **Sync risk:** HIGH · line ~53

```ini
; [BOOTSWITCH-PATCH] P2: single canonical partition table shared with CrossPoint —
board_build.partitions = ${PROJECT_DIR}/../partitions.csv
```

Upstream's own `partitions.csv` is byte-identical today (Biscuit is a CrossPoint fork);
the override guarantees it stays that way. `biscuit/partitions.csv` is left untouched
for sync hygiene.

**On conflict:** **keep ours**, then immediately `make -C tools biscuit` and watch for a
`does not fit` / region-overflow error — that means upstream grew past the shared
0x640000 slot. Never enlarge one slot asymmetrically; that breaks swapping.

## P3 — `biscuit/platformio.ini` : gate Biscuit self-OTA in dual-OS builds
**Kind:** modified · **Sync risk:** HIGH · line ~42 (in `[base]` `build_flags`)

```ini
; [BOOTSWITCH-PATCH] P3: dual-OS build — Biscuit's self-OTA would overwrite CrossPoint
  -DDUAL_OS_LOCK
```

**On conflict:** re-append the token; verify with `grep -n DUAL_OS_LOCK biscuit/platformio.ini`.
To build a single-OS Biscuit with normal OTA, remove this flag (and expect the
Check-for-updates menu entry to reappear, see P6).

## P4 — `SwapBootSlotActivity.{h,cpp}` + i18n strings : the on-device swap UI
**Kind:** **added** (2 new files + yaml block) · **Sync risk:** LOW
**Paths:** `biscuit/src/activities/settings/SwapBootSlotActivity.{h,cpp}`,
`biscuit/lib/I18n/translations/english.yaml` (5 `STR_SWITCH_OS_*` keys at EOF)

Near-verbatim copy of the CrossPoint activity (states CONFIRMING / NO_TARGET /
SWAPPING / FAILED, `bootMode` flag). Differences: Biscuit's `onGoHome()` takes no
argument, and its yaml lacks `STR_RESTARTING_HINT`, so the SWAPPING screen shows only
"Switching OS...". The yaml keys are appended WITHOUT a marker comment — Biscuit's
`gen_i18n.py` rejects comment lines in translation files.

**On conflict:** new files don't collide. If upstream ever adds a same-named file,
rename ours and update P5/P7.

## P5 — `SettingsActivity.{h,cpp}` : make the Activity reachable
**Kind:** modified · **Sync risk:** MEDIUM · 4 marker sites
- `SettingsActivity.h` — `SwapBootSlot,` added to `enum class SettingAction`.
- `SettingsActivity.cpp` — `#include "SwapBootSlotActivity.h"`; registration
  `SettingInfo::Action(StrId::STR_SWITCH_OS, SettingAction::SwapBootSlot)` after the
  Check-updates entry; dispatch `case SettingAction::SwapBootSlot:` after
  `CheckForUpdates`.

**On conflict:** the settings menu is an upstream churn point — re-add all three edits
next to the (possibly relocated) OTA entry.

## P6 — self-OTA kill switch under `DUAL_OS_LOCK`
**Kind:** modified · **Sync risk:** MEDIUM · 2 marker sites
- `biscuit/src/network/OtaUpdater.cpp` — `installUpdate()` opens with
  `#ifdef DUAL_OS_LOCK { LOG_ERR; return INTERNAL_UPDATE_ERROR; } #endif` so
  `esp_https_ota` is unreachable at runtime in dual-OS builds.
- `biscuit/src/activities/settings/SettingsActivity.cpp` — the
  `STR_CHECK_UPDATES` registration is wrapped in `#ifndef DUAL_OS_LOCK`, hiding the
  menu entry entirely.

**Why:** Biscuit's updater streams into the passive OTA slot, which in dual-OS mode
holds CrossPoint. Update Biscuit via CrossPoint's SD Card Firmware Update instead.

**On conflict:** re-wrap both sites. Verify: a `DUAL_OS_LOCK` build must not be able to
reach `esp_https_ota_begin` at runtime.

## P7 — `biscuit/src/main.cpp` : boot-hold shortcut
**Kind:** modified · **Sync risk:** MEDIUM · 3 marker sites (include, settle block, routing)

Hold **BTN_UP + POWER** at boot to open the swap screen (500 ms settle window after
`wakeupReason == PowerButton`, then `replaceActivity(SwapBootSlotActivity,
bootMode=true)` instead of home/reader routing) — same chord as CrossPoint, where
UP+DOWN+POWER means recovery instead. DOWN stays free — POWER+DOWN is the runtime
screenshot chord in both firmwares. App-level convenience, NOT a recovery path.

**On conflict:** `setup()` churns upstream; re-apply the block after the wakeup-reason
switch and the routing branch before the home/reader `if`.

## P8 — shared-data-partition safety : checked, no patch needed
Both OSes share the flashed `nvs` + `spiffs` partitions. Grep found **no**
`SPIFFS`/`LittleFS` usage (and no `begin(true)` auto-format) in Biscuit — settings and
state live on the SD card, same as CrossPoint. Re-check on every sync:
`grep -rn "SPIFFS\|LittleFS" biscuit/src biscuit/lib` should stay empty; if upstream
adds FS persistence with format-on-fail, patch it to `begin(false)`.

---

## Root-side companions (outside this prefix — never conflict on sync)
- Root `.gitmodules` carries the `biscuit/open-x4-sdk` submodule entry (upstream's own
  `.gitmodules` inside the subtree is not honored by the parent repo). Clone with
  `git submodule update --init --recursive` before building Biscuit.
- `tools/Makefile` (`B_ENV=gh_release`), `tools/sync-biscuit.sh` (upstream URL +
  `master` default branch), `bin/clang-format-fix` (excludes `biscuit/`),
  `.gitignore` (`/dist/`).

## Post-sync checklist (run after every `tools/sync-biscuit.sh`)
1. `grep -rn "BOOTSWITCH-PATCH" biscuit/` → 15 markers, every P above present.
2. Resolve `biscuit/platformio.ini` first (P1/P2/P3 per their **On conflict** notes).
3. `make -C tools biscuit` → green, and no `does not fit` (slot-overflow tripwire).
4. P8 grep still empty.
5. Re-test the swap on hardware in both directions, then commit the merge.

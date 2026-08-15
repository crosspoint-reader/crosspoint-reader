---
name: platform-targets
description: "Device and PlatformIO-env facts for CrossPoint. Use when the task mentions a board, env, FREEINK_DEVICE_*, PSRAM / BOARD_HAS_PSRAM, panel size, framebuffer, controller, touch, frontlight, uiScale, bezel insets, or 'does this work on Sticky / X3 / X4 / X4 Pro / Paper Mono'; when adding a board or compile-time cap; or when AGENTS.md defers a hardware number here."
---

# Platform Targets

`AGENTS.md` states rules that hold for every device this tree compiles. This
skill holds the numbers. One resource file per **device**, not per PIO env
name. Heap, HAL, and scope skills stay procedures; they point here for facts.

Repo language: **env** is a PlatformIO binary (`pio run -e default`). **device**
is a `FREEINK_DEVICE_*` flag. **board profile** is the SDK `BoardProfile` in
`freeink-sdk`. Do not treat those three as the same thing.

## Procedure

1. **Can build.** Parse committed `platformio.ini` for `[env:…]` and that env's
   `-DFREEINK_DEVICE_*`, `-DBOARD_HAS_PSRAM`, `-DUSE_BLOCK_DEVICE_INTERFACE`.
   Treat `*-gh_release*` as aliases of the same env class. Ignore
   `platformio.local.ini` as a contract (see below).
2. **Should build.** Intersect with CI `pio run -e` jobs in
   `.github/workflows/ci.yml`. Today that is `default` and `sticky`. If INI and
   CI disagree, say so; do not invent a third list. `pio check` is not a device
   gate (it follows the default env's include graph).
3. **Load resources.** For each `FREEINK_DEVICE_*` on a present env, read
   `resources/<device>.md` (`x3`, `x4`, `sticky`, …). Today `[env:default]`
   (and the C3 release aliases) loads **both** `x3.md` and `x4.md`. They share
   one C3 binary until that env is split.
4. **All devices this tree builds.** Union those resources. A change that
   cannot satisfy every present device without a new activity or a PSRAM-only
   heap is not a CrossPoint-wide change.
5. **Dormant files.** `resources/x4pro.md` and `resources/papermono.md` exist so
   this skill does not need a rewrite when those stanzas land. Do **not** apply
   them unless a committed env sets that flag **and** CI builds it, or the task
   is adding that env.
6. **Conflict.** If a number here disagrees with `AGENTS.md`, this skill and
   the INI win. Fix the guide; do not "correct" firmware to match stale prose.

Hardware truth is in-tree `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h`.
`https://freeink.org/llms.txt` is a website index, not `BoardConfig`.

## `platformio.local.ini`

Gitignored, desk-only, merged via `extra_configs`. It is **not** the compile
contract. Do not silently treat a local env as first-class.

If you see an `[env:…]` or `FREEINK_DEVICE_*` there that is missing from the
committed INI and is not already a first-class resource (or is only dormant),
**ask** whether to research the definition (`BoardConfig.h`, local flags) and
decide if adding or promoting a resource is right. If they say no, stop. They
can ask later.

## Maintaining resources

Field meanings live in [SCHEMA.md](SCHEMA.md). When you add or change a
committed `[env:…]` or a `FREEINK_DEVICE_*` flag, update `shared_binary_envs`
and `present_in_ini` on each affected file. When a `BoardProfile` changes,
refresh that device file. This skill may edit its own `resources/`.

## Self-review

- [ ] Used committed `platformio.ini` ∩ CI, not `platformio.local.ini`, as the
      compile set.
- [ ] Loaded one resource per device flag on those envs (`default` → x3 and x4).
- [ ] Did not apply dormant `x4pro` / `papermono` unless the env and CI exist
      or the task is adding them.
- [ ] Asked before promoting a local-only env; respected a no.
- [ ] No hardcoded 800×480 / 48KB / "no PSRAM" as project-wide facts.
- [ ] PSRAM / `MALLOC_CAP_SPIRAM` only if the env being built sets
      `BOARD_HAS_PSRAM`; C3 `default` still compiles without that allocation.

---
name: hal-and-abstractions
description: Firmware layering and abstraction discipline. Use for storage, input, activity/popup transitions, display, settings, i18n, rendering, or code that could reach the SDK.
---

# HAL and Abstractions

The [architecture rule](../../rules/architecture-hal.md) lists the HAL classes
and the SdFat-concurrency reason they exist.
This is when and how to route through them, and where to draw a new boundary.

## Route through the layer, always

- **SD card I/O:** `Storage` (HalStorage) and `HalFile`. Never `SdFat`,
  `FsFile`, `SdSpiCard`, `FsBaseFile`, or `SDCardManager` directly. The HAL
  serializes every SD access through one mutex; bypassing it races the SPI state
  machine and panics FreeRTOS (the architecture rule has the failure mode). This is a
  correctness boundary, not a style preference.
- **Display:** `HalDisplay` over `EInkDisplay`. **Input:** `HalGPIO` over
  `InputManager`.
- **Rendering:** shared FreeInkUI hosts own controls and interaction. `GUI`
  (UITheme) supplies theme metrics and shared chrome; `GfxRenderer` supplies
  drawing and oriented geometry. Follow the [UI rule](../../rules/ui-activities.md)
  for host selection. Derive layout from these contracts and the oriented
  viewable area, not hardcoded fonts, colors, coordinates, or 800/480 literals.
- **Input in activities:** `MappedInputManager::Button` logical enums
  (`Button::Confirm`, `Button::PageForward`, ...). Never raw `HalGPIO::BTN_*`
  indices outside `ButtonRemapActivity`. Logical buttons survive user remapping
  and orientation; raw indices do not.
- **Shared state:** the singleton macros (`SETTINGS`, `APP_STATE`, `GUI`,
  `Storage`, `I18N`), not threaded pointers.

For input edges, long presses, or activity/popup transitions, follow
[input frames and ownership](../../rules/ui-activities.md#input-frames-and-ownership)
through the transition checks and verification matrix before choosing a fix.

## User-facing text

Every string a user reads goes through `tr(STR_*)`. Add the key to the English
YAML, regenerate with `scripts/gen_i18n.py`, then use the `StrId`. Log lines
(`LOG_*`) stay hardcoded.

## Drawing a new boundary

When you need an SDK capability the HAL does not expose yet, **add the method to
the HAL; do not reach around it.** The new method inherits the mutex, logging,
and error contract the rest of the HAL carries. A one-off direct SDK call in an
activity is exactly the layering violation the mutex discipline cannot tolerate.

Check existing HAL methods and the pinned FreeInk SDK contract before adding
an API. Keep abstractions thin. A wrapper that only renames an SDK call without
adding the mutex, logging, or an error contract is dead weight. Add a layer only
when it carries one of those contracts or hides a real implementation choice.

## Self-review

- [ ] No direct SdFat / FsFile / SDCardManager / EInkDisplay / InputManager use
      outside `lib/hal`.
- [ ] File access uses `HalFile`; no `.close()` on a local handle
      (DESTRUCTOR_CLOSES_FILE); members closed in `onExit`.
- [ ] Input uses `MappedInputManager::Button`, not raw `BTN_*` indices.
- [ ] Controls use shared FreeInkUI hosts; chrome and drawing use UITheme and
      GfxRenderer contracts with oriented metrics.
- [ ] User-facing strings use `tr(STR_*)`; new keys added to YAML and
      regenerated.
- [ ] Any new SDK capability is exposed as a HAL method, not called inline.

---
name: hal-and-abstractions
description: Layering and abstraction discipline for the firmware. Use when touching storage, input, display, settings, i18n, or rendering, or any code that could reach into the SDK. Covers routing through the HAL (HalStorage / HalGPIO / HalDisplay) instead of raw SDK classes, MappedInputManager logical buttons instead of raw GPIO indices, UITheme/GUI for all rendering, the singleton macros, tr() for user-facing text, and where a new abstraction boundary belongs.
---

# HAL and Abstractions

CLAUDE.md lists the HAL classes and the SdFat-concurrency reason they exist.
This is when and how to route through them, and where to draw a new boundary.

## Route through the layer, always

- **SD card I/O:** `Storage` (HalStorage) and `HalFile`. Never `SdFat`,
  `FsFile`, `SdSpiCard`, `FsBaseFile`, or `SDCardManager` directly. The HAL
  serializes every SD access through one mutex; bypassing it races the SPI state
  machine and panics FreeRTOS (CLAUDE.md has the failure mode). This is a
  correctness boundary, not a style preference.
- **Display:** `HalDisplay` over `EInkDisplay`. **Input:** `HalGPIO` over
  `InputManager`.
- **Rendering:** everything through the `GUI` macro (UITheme) and the renderer's
  oriented metrics. No hardcoded fonts, colors, coordinates, or 800/480
  literals; ask the renderer for width/height and use the oriented viewable
  area.
- **Input in activities:** `MappedInputManager::Button` logical enums
  (`Button::Confirm`, `Button::PageForward`, ...). Never raw `HalGPIO::BTN_*`
  indices outside `ButtonRemapActivity`. Logical buttons survive user remapping
  and orientation; raw indices do not.
- **Shared state:** the singleton macros (`SETTINGS`, `APP_STATE`, `GUI`,
  `Storage`, `I18N`), not threaded pointers.

## User-facing text

Every string a user reads goes through `tr(STR_*)`. Add the key to the English
YAML, regenerate with `scripts/gen_i18n.py`, then use the `StrId`. Log lines
(`LOG_*`) stay hardcoded.

## Drawing a new boundary

When you need an SDK capability the HAL does not expose yet, **add the method to
the HAL; do not reach around it.** The new method inherits the mutex, logging,
and error contract the rest of the HAL carries. A one-off direct SDK call in an
activity is exactly the layering violation the mutex discipline cannot tolerate.

Keep abstractions thin. A wrapper that only renames an SDK call without adding
the mutex, logging, or an error contract is dead weight. Add a layer only when
it carries one of those contracts or hides a real implementation choice.

## Self-review

- [ ] No direct SdFat / FsFile / SDCardManager / EInkDisplay / InputManager use
      outside `lib/hal`.
- [ ] File access uses `HalFile`; no `.close()` on a local handle
      (DESTRUCTOR_CLOSES_FILE); members closed in `onExit`.
- [ ] Input uses `MappedInputManager::Button`, not raw `BTN_*` indices.
- [ ] Rendering goes through GUI/UITheme and oriented metrics; no 800/480 or
      hardcoded fonts/coords.
- [ ] User-facing strings use `tr(STR_*)`; new keys added to YAML and
      regenerated.
- [ ] Any new SDK capability is exposed as a HAL method, not called inline.

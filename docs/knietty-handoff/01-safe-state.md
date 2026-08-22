# Milestone 01 — Safe terminal-owned display state

## Objective

Make Terminal save, temporarily disable, and restore CrossPoint's sunlight
fading fix. Establish safe 20 MHz as the control artifact for every later test.

## Why first

The physical A/B test showed that the global fading fix forces window fallback
and adds an approximately 200 ms power-down path. Leaving users to change the
reader-wide setting manually makes measurements incomparable and risks changing
normal reader behavior after Terminal exits.

## Source anchors

- Global setting: [`src/CrossPointSettings.h`](../../src/CrossPointSettings.h#L256)
- Renderer initialization: [`src/main.cpp`](../../src/main.cpp#L515)
- Window fallback and async gate:
  [`lib/GfxRenderer/GfxRenderer.cpp`](../../lib/GfxRenderer/GfxRenderer.cpp#L1635)
- Terminal lifecycle:
  [`src/activities/terminal/TerminalActivity.cpp`](../../src/activities/terminal/TerminalActivity.cpp#L81)
- Safe/adaptive environments: [`platformio.ini`](../../platformio.ini#L160)

## Implementation

1. Add a renderer getter if none exists; do not reach through HAL internals.
2. In `TerminalActivity::onEnter()`, capture the renderer's effective fading
   state once, then disable it before Terminal's first terminal refresh.
3. Restore the captured state on every `onExit()` path, including failed Wi-Fi,
   denied approval, disconnect, task-creation failure, and two-press exit.
4. Do not persist the temporary value to CrossPoint settings or SD.
5. Add a small on-device/telemetry state field later so captures prove which
   effective value was used.

Prefer two booleans stored in the activity; this requires no heap allocation.
Keep restoration idempotent so partial `onEnter()` failure remains safe.

## Automated gate

- Add a native lifecycle/state test if renderer state can be isolated cheaply;
  otherwise test the new getter/setter logic and inspect all exit paths.
- Run host tests, native tests, `knietty_safe`, and `git diff --check`.
- Confirm linker RAM has not materially changed.

## Hardware gate

1. Record the user's original fading-fix setting.
2. Enter/exit Terminal with no host, after denial, after connection, and after a
   host disconnect.
3. Verify the original setting is unchanged after each exit and after sleep/wake.
4. With the setting initially on, verify Terminal window counts become nonzero
   for small updates and the fixed power-off penalty disappears.
5. Restore official 1.5.0 if sleep/wake or reader rendering regresses.

## Complete when

Safe 20 MHz builds and boots, all exit paths restore the setting, sleep/wake
still works outside Terminal, and one small-window capture proves that Terminal
used fading-fix off without permanently changing the reader preference.

# Design: Configurable Home Button Double-Click (default: toggle frontlight)

**Status:** Implemented from this spec
**Date:** 2026-08-26
**Reference:** CrossInk firmware (`uxjulia/crossink`, a CrossPoint fork) implements
this exact feature for the Xteink X4 Pro: `handleX4ProHomeKeyShortcuts()` +
`executeX4ProHomeButtonAction()` in `src/main.cpp`, tap window constant
`X4PRO_HOME_KEY_DOUBLE_TAP_MS = 300`.

## 1. Goal

On boards with a capacitive Home key (Xteink X4 Pro today; anything reporting
`BoardConfig::hasHomeKey()`), a **double-click of the Home button** performs a
user-configured action. Default: **toggle the frontlight**, matching the
already-shipped power-button double-click behavior
(`handleX4ProFrontlightDoubleClick()` in `src/main.cpp`).

Single click keeps its existing meaning (go Home / contextual back) — it is
merely delivered ~300 ms late while the firmware listens for a second click.

## 2. Behavior

| Gesture | Behavior |
| --- | --- |
| Single Home click | Delivered after the 300 ms double-click window expires with no second click. Routing unchanged (`handleHomeGesture()` → `goHome()`). |
| Second click within 300 ms | Runs the configured `homeButtonDoubleClickAction`; the pending single click is discarded. |
| Home hold | Unaffected (SDK suppresses the short tap for a hold). Cancels a pending single click if one is somehow armed. |

Actions (`HOME_BUTTON_DOUBLE_CLICK_ACTION`, persisted as `uint8_t`
`homeButtonDoubleClickAction`, default `FRONTLIGHT`):

| Value | Meaning |
| --- | --- |
| `OFF = 0` | Feature disabled; single clicks fire immediately (today's behavior). |
| `FRONTLIGHT = 1` | Toggle frontlight (guarded by `Frontlight.present()`; persists `SETTINGS.frontlightOn`). Default. |
| `GO_HOME = 2` | Same as the single-click destination (`activityManager.goHome()`). |

The setting appears in **Settings → Controls** only on boards where
`BoardConfig::hasHomeKey()` (same runtime erase-filter pattern as
`STR_SHOW_READER_MENU`), so it is also excluded from the web settings API on
boards without the key. On lightless boards choosing `FRONTLIGHT` is inert
(the toggle guards on `present()`), mirroring CrossInk.

## 3. Implementation

### Tap arbitration (main loop, CrossInk pattern)

A pending-tap state machine lives in `main.cpp` alongside the existing
power-button double-click handler:

- First click arms `pendingHomeTapAt`. While armed, the main loop consumes the
  frame (**activities do not run** — this prevents a screen from acting on the
  raw tap before the window closes; the cost is a few dozen skipped frames).
- Window expiry: disarm, then queue a **deferred home gesture**;
  `ActivityManager::loop()` delivers it through the unchanged single-click path.
- Second click within the window: disarm and dispatch the configured action.

### Files touched

1. `src/CrossPointSettings.h` — new enum + field `homeButtonDoubleClickAction`.
2. `src/main.cpp` — `handleX4ProHomeDoubleClick()` called from `loop()` before
   `activityManager.loop()`; shared `toggleFrontlightByShortcut(source)` helper
   reused by the power double-click path.
3. `src/MappedInputManager.{h,cpp}` — `queueDeferredHomeGesture()` /
   `clearDeferredHomeGesture()`; `wasHomeGesture()` returns (and clears) the
   latched gesture on Home-key boards. Non-Home-key boards untouched.
4. `src/SettingsList.h` — `buildHomeDoubleClickValues()` + Controls-category
   Enum row + `hasHomeKey()` erase filter.
5. `lib/I18n/translations/english.yaml` + regenerated `I18nStrings.*` /
   `I18nKeys.h` via `scripts/gen_i18n.py` (other languages fall back to English).
6. `src/util/HomeTapTracker.h` — pure header-only state machine
   (window = 300 ms) so the timing logic is host-testable.
7. `test/home_tap_tracker/` — host gtest suite registered in `test/CMakeLists.txt`.

## 4. Risks / notes

- **300 ms single-click latency** is inherent (CrossInk ships the same trade);
  users who dislike it set the action to `OFF`, which restores zero-latency
  clicks because the arbiter is bypassed entirely.
- Long-press-to-sleep etc. are unaffected: the SDK reports holds separately and
  suppresses the companion tap.
- Persistence: plain new settings.json key; old saves pick up the default.

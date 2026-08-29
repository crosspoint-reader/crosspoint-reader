# Design: Configurable Home Button Actions (tap / double-click / long-press)

**Status:** Implemented from this spec
**Date:** 2026-08-26
**Reference:** CrossInk firmware (`uxjulia/crossink`, a CrossPoint fork) implements
this exact feature for the Xteink X4 Pro: `handleX4ProHomeKeyShortcuts()` +
`executeX4ProHomeButtonAction()` in `src/main.cpp`, tap window constant
`X4PRO_HOME_KEY_DOUBLE_TAP_MS = 300`.

## 1. Goal

On boards with a capacitive Home key (Xteink X4 Pro today; anything reporting
`BoardConfig::hasHomeKey()`), the **Home button gestures** (tap, double-click,
long-press) each perform a user-configured action. Defaults:

- **Tap:** go back one menu level (`GO_BACK`), mirroring the X4's left-edge back
  swipe (issue #3067). The old "go home" exit-to-main-menu remains selectable.
- **Double-click:** toggle the frontlight, matching the already-shipped
  power-button double-click behavior (`handleX4ProFrontlightDoubleClick()`).
- **Long-press:** open the reader menu.

A single tap is merely delivered ~300 ms late while the firmware listens for a
possible second click; the configured *action* (not an implicit go-home) runs
when the window expires.

## 2. Behavior

| Gesture | Behavior |
| --- | --- |
| Single Home click | Delivered after the double-click window expires with no second click. Runs the configured `homeButtonTapAction` via `executeHomeButtonAction()` (default `GO_BACK` → climb one activity level, falling back to home at the top of the stack). On the device home screen the single click is a no-op, as nothing consumes it. |
| Second click within 300 ms | Runs the configured `homeButtonDoubleClickAction`; the pending single click is discarded. |
| Home hold | Unaffected (SDK suppresses the short tap for a hold). Cancels a pending single click if one is somehow armed. |

Actions (`HOME_BUTTON_ACTION`, persisted as `uint8_t`, shared by all three
gestures via `buildHomeButtonValues()`; APPEND-ONLY — values 0–2 (Off/Frontlight/Go
Home) shipped in the double-click-only version, and values 3–6 (Reader Menu/Sleep/
Screenshot/Go Back) were appended later; older saved configs keep their meaning):

| Value | Meaning |
| --- | --- |
| `OFF = 0` | Gesture does nothing. A tap set to OFF is a no-op; the arbiter is fully bypassed (zero-latency legacy routing) only when BOTH tap and double-click are OFF. |
| `FRONTLIGHT = 1` | Toggle frontlight (guarded by `Frontlight.present()`; persists `SETTINGS.frontlightOn`). Double-click default. |
| `GO_HOME = 2` | `activityManager.goHome()` — exit to the main menu. |
| `READER_MENU = 3` | `activityManager.openShortcutMenuOnCurrent()` (no-op outside the reader). Long-press default. |
| `SLEEP = 4` | Enter deep sleep. |
| `SCREENSHOT = 5` | Capture a screenshot via `ScreenshotUtil::takeScreenshot()`. |
| `GO_BACK = 6` | `activityManager.popActivity()` — climb one activity level; falls back to home at the top of the stack. Tap default. |
| `KOSYNC = 7` | Push/pull reading progress to the configured KOReader sync server (reader only — `activityManager.launchKOReaderSyncOnCurrent()`, which no-ops outside the reader, mirroring `READER_MENU`). Requires KOReader credentials to be set in **Settings → System → KOReader Sync**; if unset, the sync activity exits back to the reader. |

The settings appear in **Settings → Controls** only on boards where
`BoardConfig::hasHomeKey()` (same runtime erase-filter pattern as
`STR_SHOW_READER_MENU`), so they are also excluded from the web settings API on
boards without the key. On lightless boards choosing `FRONTLIGHT` is inert
(the toggle guards on `present()`), mirroring CrossInk.

## 3. Implementation

### Tap arbitration (main loop, CrossInk pattern)

A pending-tap state machine (`HomeTapTracker`) lives in `main.cpp` alongside the
existing power-button double-click handler:

- First tap arms `homeTapTracker`. While armed, the main loop consumes the frame
  (**activities do not run** — this prevents a screen from acting on the raw tap
  before the window closes; the cost is a few dozen skipped frames). The raw
  Home-key edge is sampled every frame and only frames carrying the event are
  intercepted, so unrelated input is never delayed.
- Window expiry: disarm, then run the configured `homeButtonTapAction` directly
  through `executeHomeButtonAction()` (the same dispatcher used by double-click
  and long-press). No deferred gesture is queued.
- Second click within the window: disarm and dispatch the configured
  `homeButtonDoubleClickAction` via `executeHomeButtonAction()`.

### Files touched

1. `src/CrossPointSettings.h` — `HOME_BUTTON_ACTION` enum (8 values: appended `KOSYNC = 7`) + fields
   `homeButtonTapAction` / `homeButtonDoubleClickAction` / `homeButtonLongPressAction`.
2. `src/main.cpp` — `handleX4ProHomeDoubleClick()` called from `loop()` before
   `activityManager.loop()`; `executeHomeButtonAction()` runs the selected action;
   shared `toggleFrontlightByShortcut(source)` helper reused by the power double-click path.
3. `src/util/HomeTapTracker.h` — pure header-only state machine
   (window = 300 ms) so the timing logic is host-testable.
4. `src/SettingsList.h` — `buildHomeButtonValues()` (shared 7-value list) +
   three Controls-category Enum rows + `hasHomeKey()` erase filter.
5. `lib/I18n/translations/english.yaml` + regenerated `I18nStrings.*` /
   `I18nKeys.h` via `scripts/gen_i18n.py` (other languages fall back to English).
6. `test/home_tap_tracker/` — host gtest suite registered in `test/CMakeLists.txt`.

## 4. Risks / notes

- **300 ms single-click latency** is inherent (CrossInk ships the same trade);
  users who dislike it set the tap action to `OFF` *and* the double-click action
  to `OFF`, which bypasses the arbiter entirely for zero-latency routing.
- Long-press-to-sleep etc. are unaffected: the SDK reports holds separately and
  suppresses the companion tap.
- Persistence: plain new settings.json keys; old saves pick up the defaults.
  Appending `GO_BACK = 6` (rather than renumbering) keeps 0–5 stable for devices
  that already persisted a tap/double-click/long-press choice. `KOSYNC = 7` is
  appended last, so existing saves (which store at most 6) never collide and
  remain valid under the generic enum-size clamp.

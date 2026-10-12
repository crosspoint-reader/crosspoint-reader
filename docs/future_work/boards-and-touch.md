# Boards and touch

Status: open items as of 2026-10-07. Collected from multiboard-bringup-handover-2026-08-15.md, multi-board-bringup-2026-08-14.md, x4pro_support.md and touch-input-migration-2026-08-14.md.

Background for all of these is in [Board Support](../contributing/board-support.md) and
[Touch Architecture](../contributing/touch-architecture.md). Display items from the same sources
(the LGFX write-buffer copy before `displayGrayBuffer()`, the pre-render waiting for the deferred
AA pass, the PanelSel/baseline-model split) are in [Display and refresh](display-and-refresh.md).

## Light sleep with the frontlight

Open: `HalPowerManager::lightSleep()` refuses to sleep on any board with a PWM frontlight
(`BoardConfig::hasPwmFrontlight()`, counted in `rejFrontlight`), whether the light is on or not.
Both touch boards have one, so the X4 Pro and the T5S3 never take idle light sleep. The comment
above the guard is stale: it says the fork has no frontlight driver and that no board we build has
a frontlight. `HalFrontlight` exists, with `present()` and `isOn()`.

Why it matters: idle light sleep is the main idle-power saving, and a dark frontlight needs no
guard. Light sleep stops the LEDC clock, so only a lit PWM light flickers.

Where: `HalPowerManager::lightSleep()`, `HalFrontlight`, `[x4pro_board]` in `platformio.ini`, and
the SDK's `FrontlightManager` (`FREEINK_FRONTLIGHT_LS`, `park()`, `releaseOnWake()`). The comment
beside the `lightSleep()` fallback in `main.cpp`'s idle loop lists the reasons it declines without
the frontlight.

Ruled out: porting upstream's exclusion as the design. crosspoint-reader
[PR #3032](https://github.com/crosspoint-reader/crosspoint-reader/pull/3032), with
[Free-Ink/freeink-sdk#39](https://github.com/Free-Ink/freeink-sdk/pull/39), clocks the frontlight
LEDC from RC_FAST with KEEP_ALIVE so the PWM survives light sleep; under `FREEINK_FRONTLIGHT_LS`
the lit-light clause compiles out. The submitter measured 92 % light-sleep residency with the
light on (X4 Pro). Our SDK carries the driver half; the app half is not ported.

Next: (1) tighten the guard to `Frontlight.present() && Frontlight.isOn()` and rewrite the
comment; (2) port the app half for the X4 Pro: set `FREEINK_FRONTLIGHT_LS` in `[x4pro_board]`,
call `park()` on the way into deep sleep and `releaseOnWake()` before `Frontlight.begin()`, then
compare `rejFrontlight` and the light-sleep residency with the light on and off. The flag is
untested on the T5S3.

## SD clock on the T5S3

Open: `HalStorage::begin()` holds SD-over-SPI at 20 MHz on non-C3 boards whose profile leaves
`sd.spiHz` at 0, instead of the SDK's 40 MHz default. Today that is only the T5S3; the X4 Pro
mounts over SDMMC.

Why it matters: if the clamp is not needed it halves SD throughput on the T5S3.

Tried: nothing; the clamp was a precaution. The SD card may have worked at bring-up because
`BoardT5S3::begin()` deselects the LoRa radio on the same bus, not because of the lower clock.

Next: one T5S3 build at 40 MHz, then a book open, a section build and a USB-drive transfer. Keep
the clamp only if mounts or reads fail.

## BUTTON_TRACE scaffolding

Open: `-DBUTTON_TRACE=1` is commented out in `[lilygo_board]`, but its code remains in
`HalGPIO::sampleOnce()` (raw button, Home key and touch-release lines), `GestureEventManager`,
`TouchGestures.h` and `ActivityManager.cpp`, and its touch-edge state (`touchTraceWasHeld_`,
`touchTraceDownNx_` and friends) sits unguarded in `HalGPIO.h`. The `platformio.ini` comment still
says to drop it once the board's input is settled.

Why it matters: it is either a maintained diagnostic or dead code in the input path, and today it
is described as neither.

Next: decide. To keep it, reword the `platformio.ini` comment the way the `LGFX_EPD_PUSH_TRACE`
comment beside it is worded and check that a trace build compiles;
[Touch Architecture](../contributing/touch-architecture.md#debugging-touch) already describes the
output. To drop it, delete the trace blocks in those four files and the state in `HalGPIO.h` in
one commit.

## FREEINK_X4PRO_FAST_DU_SHORTCUT

Open: the SDK's opt-in fast-DU refresh for the X4 Pro (`Ssd1677Driver.cpp`) is not enabled in
`[x4pro_board]`.

Why it matters: it saves about 85 ms per refresh. It is off because it skips the per-refresh
temperature load and power sequencing, and the C3 X4's equivalent (`FREEINK_X4_FAST_DU_SHORTCUT`,
also off in `[c3]`) produced ghosting and blotching over long sessions on some panels upstream.

Constraint: it applies only to SSD1677-batch X4 Pros. UC8179 and UC8279 batches use other drivers
and never reach it, and the X4 Pro on the bench is a UC8179 unit.

Next: an SSD1677-batch X4 Pro and a long reading session across temperatures, before enabling it.

## Device validation

Open: parts of the touch layer and of the X4 Pro have no recorded device pass.

- The touch defaults in [Touch Gestures](../touch-gestures.md) have had one hardware session, on a
  T5S3 (2026-09-11). It found four faults, all fixed since. Whether the either-direction ten-page
  swipe is now reliable, and whether the gesture overview's labels fit legibly, were not checked
  again.
- The X4 Pro has run this firmware since the pre-flight fixes (PR #246, 2026-09-12), and display
  work has been tested on it since. Earlier plans said it was deliberately not flashed; that hold
  ended with PR #246. There is still no recorded pass for its gesture set (the warm/cool right edge
  column exists only there), two-finger gestures on its GT911, or the input sampler's stack
  high-water with its GT911 (the 4096-byte stack was measured only on the T5S3).

Why it matters: host tests cover geometry, not what a thumb can reach or what a screen shows. The
first T5S3 session found four faults that 880 host tests and two board builds had passed.

Next: one session per touch board against the tables in [Touch Gestures](../touch-gestures.md)
and the on-device Gesture overview, built with `BUTTON_TRACE`; on the X4 Pro also read the
`btnSampler stack high-water` line.

## Localized tap flash

Open: screens on the recorder path get no tap acknowledgement except the selection repaint
(`ActivityManager::dispatchListTap()`, `Selected` branch). Those are the screens still on
`ListTouchBand`: the EPUB and XTC chapter selectors, the Markdown table of contents, footnotes,
the OPDS browser, Wi-Fi selection, global bookmarks, starred pages and KOReader sync
(`ButtonRemapActivity` is excluded on purpose).

Already done elsewhere: screens on FreeInkUI (`UiListActivity`, `TabbedUiListActivity`,
`UiAppHost`) get FreeInkUI's tap flash, which paints the tapped element gray in the refresh that
shows the result (`FreeInkApp::route()`).

Ruled out: a flash with its own partial refresh bounded to the row. The SDK primitive it would have
used, `ListNav::rowRectFor()`, was removed upstream, and a windowed refresh on the SSD1677 and
UC8279 costs nearly as much as a full one (measurements in
[Display and refresh](display-and-refresh.md#windowed-refresh-capability-query-alignment-baseline)).

Next: move the remaining band screens onto `UiListActivity`, which brings the flash with it,
rather than building a second flash for the recorder path.

## Ten-page skip stops at a chapter boundary

Open: `EpubReaderActivity::jumpPages()` calls `stepPageStateLocked()` up to ten times. Crossing
into the next or previous spine item resets `section`, and the following step returns false
because there is no section, so "skip 10 pages" stops at the chapter boundary.

Why it matters: the outer-third swipe defaults (`gestSwipeInLeftZone`, `gestSwipeInRightZone`)
and the double-press button defaults (`btnDoubleLeft`, `btnDoubleRight`) all use these actions.

Ruled out: it is not why the ten-page swipe failed on the left side on the T5S3; that was
ergonomic, and the swipe now accepts either direction.

Next: carry the remaining count across the spine (for example, resolve a page offset once the new
section has loaded), or declare the stop intended and say so in
[Touch Gestures](../touch-gestures.md). Only the EPUB reader stops: the TXT, Markdown, XTC and line
readers' `BTN_PAGE_*_10` handlers move a whole-document page index by ten and clamp.

## No tappable scroll bar on lists

Open: no list has a tap target on its scroll indicator. Paging by tapping beside the thumb
(`ListScrollBar`, `ActivityManager::dispatchScrollBarTap()`) was removed when the lists moved to
FreeInkUI (cabc632e0, 2026-09-14). FreeInkUI's `list()` registers only rows as hit targets, and the
lists `BaseTheme` still draws show overflow arrows (`drawListOverflowArrows()`) with no target. Paging
by touch is a vertical swipe or a hint box; neither [Touch Gestures](../touch-gestures.md) nor the
user guide offers a scroll-bar tap.

Why it matters: the T5S3 has no Up key, and a scroll-bar tap was the most direct way back up a long
list.

Next: decide whether to add a scroll-track interaction to FreeInkUI's list (an SDK change that would
cover every FreeInkUI list).

## Long-press discoverability

Open: nothing on screen tells a touch user that a hint box can be held. A long tap on a hint box is
the only way to hold Back, Confirm, Left or Right on the touch boards
(`ActivityManager::dispatchHintStripTap()`), and some actions exist only as a hold, for example
long Back to go Home from a list screen (`UiListActivity`).

Partly done: `FileBrowserActivity` names most of its hold actions in the "short / long" form:
"» / Options" (`STR_LIST_PAGE_NEXT_OR_OPTIONS`) where Options is the hold of Right, "Right / Options"
in the cover grid, and in the Library "Up / Tab" / "Down / Tab" on the side hints
(`STR_DIR_UP_OR_TAB`, `STR_DIR_DOWN_OR_TAB`). The gesture overview marks taps and holds for the
reader zones. The browser still has two unnamed holds: long Back goes Home from a Books folder or an
opened author (the box says Back), and long Confirm on an EPUB opens it with a KOReader pull when
credentials are set (the box says Open).

Next: apply the browser's "short / long" form wherever a screen has a hold-only action.

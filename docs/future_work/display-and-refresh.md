# Display and refresh

Status: open items as of 2026-10-07. Collected from display-baseline-audit-2026-09-23.md, display-capability-audit-2026-08-17.md, framebuffer-content-audit-2026-09-23.md, x3-sleep-ghosting-hypotheses.md, superpowers/specs/2026-09-23-framebuffer-contract-design.md, busy-indicator-handover-2026-09-21.md, background-rendering.md and grayscale-aa-rendering.md (the audits and handovers were removed in the 2026-10-07 docs cleanup; they are in git history).

Background for every item: [secondary-buffer-management.md](../secondary-buffer-management.md) (what each
framebuffer holds, and where each controller keeps its previous frame).

## Greys on the glass: UC8279d and SSD1677 do not track them

**Open.** After an anti-aliased page, `Uc8279Driver::cleanupGrayscaleBuffers()` and
`Ssd1677Driver::cleanupGrayscaleBuffers()` restore controller RAM to the B/W page and report a clean
sync, while the glass still holds grey at every AA edge. UC8253 had the same gap and it caused the X3
sleep-cover ghost; it now tracks the state as `_grayOnGlass`. UC8179 and UC8279 X4 track it as
`_redriveAfterGray`. SSD1677 tracks it after a factory (absolute) pass only: `displayGray()` sets
`_needsGrayClear`, which promotes the next FAST; the external-LUT (overlay) pass sets nothing.

**Why it matters.** A differential push after an AA page gives those grey pixels the gentle
white-to-white cell. On UC8253 that left the page's text outline on the sleep cover.

**Where.** `Uc8279Driver::displayGrayscaleBase()` (`cleanBaseNeeded`), `Uc8279Driver::displayGray()`,
`Ssd1677Driver::cleanupGrayscaleBuffers()`.

**Ruled out.** SSD1677's sleep cover is safe: its absolute pass is `GrayscaleBase::Combined` (no base
push, the factory waveform drives every pixel), confirmed clean on an X4 on 2026-09-23. UC8279d was
never tested; there is no unit.

**Next.** On a UC8279d X3 (log tag `8279_DRF`), read a few AA pages and sleep onto a grayscale cover.
If the text ghosts, port `_grayOnGlass` (set by the gray waveform, cleared only by a GC push or a
revert, never by a RAM restore) with a host test in `run_uc8279.py`. An alternative for every
UltraChip driver is UC8179's rule: a Half or Full fallback in `displayGrayscaleBase()` is a floor,
never the differential transition.

## Re-landing the reverted framebuffer fixes

**Open.** Five host-buffer defects (D1-D5) were found by code reading on 2026-09-23, fixed on a branch,
flashed, and reverted when they turned out not to cause the X3 sleep ghost. D1 was re-landed (SDK
`4a320d6`: `cleanupGrayscaleWithPreviousBuffer()` accepts only `frameBufferActive`). D2-D5 are the four
items below.

**Why it matters.** Each one composites or saves the wrong frame in a specific state. None of them was
the observed symptom, so none has device evidence yet.

**Lesson.** The first fix was chosen from a story, not from a test that separates causes, and it
changed nothing. Re-audit each defect against its own trigger before re-landing it.

**Where the written fixes are.** Only as unreachable commits: firmware `c17d939ab` (a docs commit;
the fixes are its ancestors `a6276195f` D2, `1989118a6` D5, `195bcd5d4` D3 and `d2ddab827` D4), SDK
`96f031d` (D1) and `08222e1` (D5). No branch holds them, so `git gc` will eventually drop them.

**Next.** Tag the commits if the code is wanted; otherwise rewrite from the descriptions below.

## D2: the reader's exit hook repairs only the pre-render case

**Open.** `EpubReaderActivity::prepareFramebufferForCapture()` calls
`restoreCurrentPageToBufferIfPreRendered()`, which returns unless `preRenderedPage.ready`. It does not
repair the ordinary post-swap state (the write buffer holds the previous page), nor a plane left behind
when a grayscale pass ran with the secondary away (`cleanupGrayscaleWithPreviousBuffer()` then restores
nothing). `LineReaderActivity` and `XtcReaderActivity` do not override the hook at all.

**Why it matters.** The Overlay and Quick Resume sleep screens, the frontlight drawer and screenshots
all draw on top of the write buffer.

**Tried.** `restoreCurrentPageToBufferIfStale()`, which also fires when no displayed frame is
available. Reverted with the rest.

**Next.** Decide together with "Framebuffer contract" below, which is the structural version.

## D3: goToSleep() prepares nothing (narrowed)

**Open.** `ActivityManager::goToSleep()` replaces the activity and pumps `loop()` without the
`syncWriteBufferFromDisplayed()` + `prepareFramebufferForCapture()` pair that
`dispatchLightPanelGesture()` runs. `SleepActivity::renderOverlaySleepScreen()` and
`renderLastScreenSleepScreen()` then draw onto whatever the write buffer holds. From an EPUB, the
replace runs `EpubReaderActivity::onExit()`, which calls `restoreCurrentPageToBufferIfPreRendered()`,
so there only the sync half is missing.

**Narrowed.** With AA on it is correct: every completed or aborted AA pass ends in
`cleanupGrayscaleWithPreviousBuffer()`, which copies the displayed frame into the write buffer (X3,
2026-09-24: Overlay sleep after two quick turns showed the current page). Still possible with AA off,
or for a sleep in the ~0.5 s before the deferred AA pass runs.

**Tried.** `goToSleep()` running the pair under a `RenderLock` before the pump. Reverted.

**Next.** Repeat the test with AA off. Re-land only if it shows the previous page.

## D4: Quick Resume saves the post-swap slot

**Open.** `SleepActivity::renderLastScreenSleepScreen()` draws the moon and calls
`displayBuffer(HALF_REFRESH)`, which swaps. `saveSleepFrameBuffer()` in `main.cpp` then writes
`renderer.getFrameBuffer()`, which is now the other slot: the page without the moon (or, with AA off,
the page before), not what the panel shows.

**Why it matters.** On wake, `loadSleepFrameBuffer()` restores that frame as the reader page.

**Tried.** Nothing on a device. The wake check (does the restored frame show the moon?) never ran.

**Next.** Run the check. The fix is to call `syncWriteBufferFromDisplayed()` before the write.

## D5: a resident secondary can hold a frame that was never displayed

**Open.** `FreeInkDisplay::returnSecondaryBuffer()` and `reallocSecondaryBuffer()` seed the restored
secondary from the write buffer, which may hold a pre-rendered page, a plane or scratch.
`hasSecondaryBuffer()` then reports true, and `syncWriteBufferFromActive()` copies the block without
saying whether it is the panel content. The `_redBaselineAuthoritative` one-shot protects the SSD1677's
RED plane, not the host copy.

**Why it matters.** A popup or overlay drawn with `overlayDisplayedFrame=true` between the return and
the next full-frame refresh composites onto that frame.

**Tried.** `_displayedFrameValid`, set only by `swapBuffers()` and exposed as `hasDisplayedFrame()`,
with `syncWriteBufferFromActive()` returning whether it copied. Host-tested, reverted (SDK `08222e1`).

**Next.** List the consumers that can run between a return or realloc and the next full-frame refresh.
Re-land only if one exists.

## Framebuffer contract: one routine and one hook

**Open.** The rule "before drawing on top of the panel, the write buffer must hold what the panel
shows" lives at each call site, and each does it differently:
- `ActivityManager::dispatchLightPanelGesture()` runs `syncWriteBufferFromDisplayed()` and then
  `prepareFramebufferForCapture()`. Its comment says the screenshot path does the same; it does not.
- The POWER+DOWN screenshot path in `main.cpp` runs only `prepareFramebufferForCapture()`, so
  outside the reader's pre-render case it saves whatever the write buffer holds (see D2). The serial
  `CMD:SCREENSHOT` path runs neither.
- The `overlayDisplayedFrame` popups (`BaseTheme::drawPopup()`, `drawBusyIndicator()`,
  `LyraTheme::drawPopup()`) run only the sync, as do the screens that call
  `syncWriteBufferFromDisplayed()` themselves (Home, the file browser, book info, the sleep cover,
  the font selector, the frontlight panel, dictionary word select, the reader shell, and the forced
  refresh in `main.cpp`).

**Agreed design, not scheduled.**
1. `Activity::ensureWriteBufferShowsPanel()` plus an `ActivityManager` delegate: run the two steps in
   one place and return false when the frame cannot be established, so the caller repaints instead of
   compositing.
2. Replace `Activity::prepareFramebufferForCapture()` with `virtual bool redrawVisibleFrame()`, default
   false. `EpubReaderActivity` answers it unconditionally; Txt and Md through
   `TxtReaderActivity::drawCurrentPageToBuffer()`; Xtc through its own static.

Neither exists in the tree. Rejected on purpose: an RAII guard (nothing to undo), new `drawPopup()`
signatures, and an SDK type for "the displayed frame".

**Next.** Mechanism 2 first, then 1 with the call-site migration. No host harness compiles
`GfxRenderer`, so the migration is reviewed by eye.

## Driver conformance test for the no-baseline contract

**Open.** Every driver must treat `cleanupGrayscaleBuffers(bus, nullptr)` as "no baseline: take a clean
sync on the next push". They all do today, each in its own way (UC8253 drops `_redRamSynced`, UC8179
and UC8279 X4 set `_needFullClear`, UC8279d sets `_forceFullSyncNext`, SSD1677 leaves
`_inGrayscaleMode` set, LGFX rebuilds its canvas on the next push). Only
`test_pro.cpp::testGrayCleanupBaseline` checks it, and only for `Uc8279X4Driver`.

**Why it matters.** A new controller that keeps its synced claim ships ghosting.

**Next.** In the SDK host suite, for each linked driver: run a grayscale pass, call
`cleanupGrayscaleBuffers(nullptr)`, assert that the next refresh is a clean or full sync.

## Make the display state queryable

**Open.** Three states decide what is safe to draw or push, and today they are implicit or private:
1. what the write buffer holds: the panel, the next page (`preRenderedPage.ready`), a plane (nothing
   records it), a stale frame (the post-swap default) or scratch;
2. what the secondary holds: the panel, an unproven seed (after a return or realloc), lent
   (`_secondaryLent`, private) or released;
3. whether the glass carries grey the B/W baseline does not describe: per driver and private
   (`_grayOnGlass`, `_redriveAfterGray`, SSD1677's `_needsGrayClear` after an absolute pass only),
   absent on UC8279d.

**Why it matters.** `ActivityManager::showBusyIndicator()` skips whenever the secondary is away (unless
something earlier in the tick already prepared the write buffer), because `hasSecondaryBuffer()` cannot
tell a borrow from a release. A consumer about to push a differential after
a grey page (sleep cover, Home after the reader, a T5S3 FAST) cannot ask about state 3.

**Not a design yet.** States 1 and 2 are host-side and cheap. State 3 decides the refresh mode and
must be answered by each driver, so keep it separate from the other two.

**Next.** Add `FreeInkDisplay::isSecondaryLent()` with `HalDisplay` and `GfxRenderer` passthroughs, and
use it in `showBusyIndicator()`. Design state 3 when a second consumer needs it.

## Split PanelSel from the baseline model

**Open.** `FreeInkDisplay::PanelSel` (`X4`, `X3`, `M5`; default `X4`) picks the driver and also stands
for "this controller keeps a host-managed previous frame", which is true of the SSD1677 only. Every
board that is not an X3 reads as `X4`: X4 and X4 Pro units on UC8179 or UC8279, and the T5S3.
- `resolveReleasedMode()` turns FAST into HALF while the secondary is away (unless
  `setSingleBufferFastDiff(true)`), also on drivers that ignore `prev` and keep their own baseline.
- `isRedRamSynced()` reports a RED plane that UC8179, UC8279 X4 and LGFX do not have.
- `syncRedRamFromFrameBuffer()` runs there; harmless only because `seedPreviousFrame()` is a no-op
  outside SSD1677.
- The firmware no longer asks by board name: it asks the facade's own rules,
  `fastRefreshNeedsHostFrame()` (build mode, RED seeds, post-popup HALFs) and
  `asyncRefreshHoldsFrameBuffer()` (deferred AA). Both are still keyed on `PanelSel`, so fixing them
  here fixes every firmware decision at once.

The SDK's `displayCommitted()` and `abortPostRefresh()` / `postRefreshAborted()` are still unused: the
reader hand-rolls `aaPreemptedByNavigation()`, and the refresh cadence assumes every submit commits.
Also open: whether the PSRAM boards (X4 Pro, T5S3) need to borrow or release the secondary at all.

**Why it matters.** A downgraded FAST costs a HALF waveform: on an X4, ~1755 ms instead of ~476 ms
(2026-08-19, during a Background-B borrow).

**Next.** SDK: a `PanelDriver` capability for "consumes a host-managed previous frame" (true where the
driver uses `prev`), and answer `fastRefreshNeedsHostFrame()` and `asyncRefreshHoldsFrameBuffer()` from
it instead of `PanelSel`. That changes behaviour on the UC8179/UC8279 X4-family units and the T5S3
(no needless FAST-to-HALF downgrades, released builds), so it wants a device check on each.

## LGFX write-buffer copy before displayGrayBuffer()

**Open.** `HalDisplay::displayGrayBuffer()` copies the displayed frame into the write buffer when the
controller is `LgfxEpd`, a board-name check. Since Free-Ink/freeink-sdk#47 `LgfxEpdDriver::displayGray()`
ignores `fb` and darkens its own canvas, so the copy no longer serves the purpose it was written for.

**Why it was kept.** It touches the host write buffer, not the canvas, and whether the plane-restore
step after it depends on it was never settled on the panel. Cost: one buffer copy per two-push AA pass
on the T5S3, which now mostly uses single-push AA.

**Next.** Remove it behind a device test that exercises the two-push overlay path on a T5S3.

## Windowed refresh: capability query, alignment, baseline

**Open.** `PanelDriver::displayWindow()` defaults to a whole-panel FAST. Only `Ssd1677Driver` and
`PaperMonoDriver` override it, and both return silently on a window whose x or width is not
byte-aligned. A caller cannot tell "refreshed the window", "refreshed the whole panel" and "did
nothing" apart, and there is no `supportsWindowedRefresh()`. After a windowed refresh
`frameBufferActive` still holds the old content for that rectangle (no swap), so the next FAST can
leave those pixels as the window left them. `GfxRenderer::displayWindow()` has no caller today.

**Ruled out.** On the SSD1677 a windowed refresh is not worth having. X4, 2026-09-16: 88 of 480 rows
took 505-511 ms against 527-529 ms for a full-frame FAST, and it ghosted; built, measured, reverted.
Free-Ink/freeink-sdk#91 measured the same on a UC8279 (32 rows 514 ms, 480 rows 556 ms).

**Next.** Only if a driver turns out to save real time (next item): add `supportsWindowedRefresh()`
(false in the base) and surface it like `supportsAsyncRefresh()`; snap to byte alignment in
`GfxRenderer::displayWindow()`, because the constrained axis flips with orientation; fall back to a
full refresh instead of returning; copy the refreshed rectangle into `frameBufferActive`.

## displayWindow() on UC8253 and UC8179

**Open.** `Uc8253X3Driver` and `Uc8179Driver` already drive the partial-window commands (PTL `0x90`,
PTIN `0x91`, PTOUT `0x92`): UC8253 in its windowed `preconditionGrayscale()`, UC8179 as a whole-panel
PTIN on FAST. Neither overrides `displayWindow()`.

**Why it matters.** A real partial would make popups, the busy indicator, the frontlight drawer and the
status bar cheaper.

**Known constraint.** An earlier X3 (UC8253) experiment, from before the FreeInk SDK: a PTL window
narrower than the full source range speckled about one refresh in ten, with every LUT bank; a
full-width window limited only in gate rows was clean 9/9. In portrait only a logical left or right
edge strip has that shape; a status bar does not.

**Unmeasured.** Whether a row-limited window saves waveform time on these parts at all. The SSD1677
and UC8279 measurements above say it may not.

**Next.** Measure before writing the override: a full-width 60-row window against a whole-panel FAST,
on an X3 and on an X4 Pro with UC8179.

## PopupShip::Window

**Open.** `BaseTheme::drawPopup()` and `drawBusyIndicator()` return the box `Rect`, but `PopupShip` has
only `Blocking`, `Async` and `Caller`, so every popup ships a full-frame refresh. The busy indicator is
limited to panels whose FAST takes under `kMaxFastRefreshMs` (1000 ms) for that reason.

**Depends on.** A driver whose windowed refresh saves time (item above).

**Next.** Add `PopupShip::Window`, which ships the returned `Rect` through `GfxRenderer::displayWindow()`.

## Retire the PopupShip::Async drain caveat

**Open.** `FreeInkDisplay::releaseSecondaryBuffer()` drains a pending refresh since SDK `d939fd1`, but
two firmware comments still say it does not: the `PopupShip::Async` comment in `BaseTheme.h`, and the
comment in `SleepActivity::renderCoverSleepScreen()` that explains why the popup is armed only after
the release.

**Next.** Correct both comments. The arm-after-release order is harmless and can stay.

## drawImage() does not rotate bitmap bits

**Open.** `GfxRenderer::drawImage()` moves the origin for the current orientation but does not rotate
the bitmap (`// TODO: Rotate bits`). A bitmap that is not rotation-symmetric lies on its side outside
the panel's native orientation. The busy indicator draws its hourglass with `drawLine()` to avoid this.

**Where.** Callers: `BootActivity`, `SleepActivity` and `SystemInformationActivity` (`Logo120`),
`SleepActivity::renderLastScreenSleepScreen()` (`MoonIcon`), the Quick Resume path in `main.cpp`
(`LoadingIcon`).

**Next.** Rotate the bits per orientation in `drawImage()` and check each caller in all four
orientations.

## The Fast anti-aliasing setting does nothing

**Open.** `CrossPointSettings::fastAntiAliasing` (offered on every non-SSD1677 panel, see
`hasSelectableGrayscaleLut()`) reaches `FreeInkDisplay::setFastGrayscaleLut()`, which only stores the
flag. No driver reads it: `Uc8253X3Driver` carries a `gc` nudge bank and a `directGray` bank (for
`GrayscaleMode::Direct`), neither chosen by the flag, and `HalDisplay::displayGrayBuffer()` passes no
LUT. The comment above the setting in `SettingsList.h` still promises a choice between ~2.4 s and
~130 ms, and so do the comments at `CrossPointSettings::fastAntiAliasing` ("2.2 s/page"),
`GfxRenderer::setFastGrayscaleLut()` and `HalDisplay::setFastGrayscaleLut()` (53- against 7-frame LUT).

**Next.** Either give the X3 driver a second grayscale bank selected by the flag, or remove the setting
and those comments.

## X3 and the pre-render heap floor

**Likely resolved: confirm, then delete this item.** On 2026-09-23 Background A (next-page
pre-render) did not run on the X3 (UC8253, AA on, 17 turns). Free heap after each AA pass was
~42.9 KB, and every page logged `PreRender skipped: free=… < floor=45056`.
`PRE_RENDER_MIN_FREE_HEAP_BYTES` (44 KB) was derived on 2026-08-02 (commit `6f097ad77`), when the X3
entered the pass at 53-55 KB free.

On 2026-10-08 it ran (X3, AA on, one session in one book). After each deferred AA pass,
`Reader mem[prerender_begin]` showed 58.5 KB free, the pass ran, and a turn found its page
pre-rendered. What brought the retained baseline back down was not identified.

**Why it matters.** Without A every turn pays the page render on top of the waveform.

**Ruled out.** Lowering the floor. It is derived from what the pass consumes (~23 KB transient), and
the pass must stay clear of the reserve the sliced section build needs.

**Next.** Watch `Reader mem[prerender_begin]` and `PreRender skipped: free=… < floor=…` (a debug
line in `renderPreRenderPass()`) across a few books and a longer session. The re-arm in
`stepBackgroundSectionBuild()` checks the floor too but logs only through
`HEAP_GATE("preRenderArm", …)`, which is compiled out unless `HEAP_GATE_TRACE=1`; build with it, or a
re-arm the floor refuses goes unseen. If no page skips for the floor, delete this item. If it recurs, find
what grew: bisect from `6f097ad77` on one book, using the `Reader mem[...] ... allocBytes=` line in
`EpubReaderActivity` as the metric.

## Pre-render before the deferred AA pass

**Open.** `EpubReaderActivity::serviceBackgroundWork()` runs the deferred AA pass first. Background A
is armed by `renderContents()`, but on a deferred-AA panel its pass is held back while the AA is owed
(`PreRender deferred: AA owed` in `render()`) and re-requested when the AA has run. The AA's cleanup
(`cleanupGrayscaleWithPreviousBuffer()`) copies the current page back over the write buffer, which
would wipe a pre-rendered next page, and a render requested while AA is owed makes
`aaPreemptedByNavigation()` abort the AA. A quick turn inside the AA window pays a full render. Measured on a T5S3 on 2026-08-17, when it still deferred AA:
AA ~605 ms (planes 80 + gray 473 + restore 52), pre-render ~53 ms; 2 of 6 quick turns found a
pre-rendered page.

**Status.** The T5S3 now sends AA as a single push in normal reading, so the measured case no longer
happens there. The X3 is the board left on the deferred path, and A runs there again as of
2026-10-08 (item above). That log shows the order on every page: the deferred AA (planes ~296 +
gray ~357 + restore ~50 ms), then the pre-render (~70 ms).

**Next.** Measure how often a quick turn on the X3 lands inside the AA window. Reordering needs a C3
heap check and a device test, not an inference from one log.

## Pre-render across a chapter boundary

**Open.** Background A never crosses a section. `EpubReaderActivity::renderPreRenderPass()` skips when
`nextPage >= availablePages`. All four arming sites need a next page in the current section:
`renderContents()`, `renderBufferDisplayPass()` and the re-arm in `stepBackgroundSectionBuild()` test
`currentPage + 1 < pageCount`, and the image lane's re-arm in `warmPageForImageLane()` (#406) arms
only for page `currentPage + 1` of the current spine. So the first page of every chapter renders
fresh, including a chapter-opening illustration.

**Why it matters.** X4, 2026-10-08, turn into a chapter whose first page has a cached image:

| Step | Time |
|---|---|
| Section load | 35 ms |
| Image size probe | 32 ms (removed by #407: `ImageBlock::wouldShowPlaceholder()` checks the cache first) |
| Font prewarm | 36 ms |
| BW render | 68 ms |
| Waveform + inline AA | ~775 ms |

A pre-render would bring the first pixels forward by ~105 ms, from ~146 ms after the press to
~40 ms. That happens once per chapter. On the X4 the turn as a whole finishes at about the same
time: a pre-rendered page runs its AA after the waveform instead of overlapping it. The expensive
part, the decode, is already off the turn, because the image lane warms the next section's first
pages.

**Deferred** 2026-10-08: the gain is small for the change it needs.

**Design, if picked up.**
1. Arm on a section's last page when a next spine exists, at all four sites above.
2. Pre-render (spine + 1, page 0). Use Background-B's `backgroundSection_` when it holds that spine
   complete; otherwise load the section the way the image lane's spill into the next section does.
   Skip the pass unless `buildSection()` would use that cache as it is.
3. Extend the fast path in `pageTurn()`. On the last page, with that pre-render ready, make the same
   crossing `stepPageStateLocked()` makes (`navTarget` to page 0, `currentSpineIndex++`,
   `section.reset()`) and hand the buffer over through `usePreRenderedBuffer`.
4. In the BufferDisplay pass, when there is no section yet, run `buildSection()` first. On a cache
   hit it reads the LUT and draws nothing. Show the buffer only if it hit and resolved page 0.
   Anything else falls back to the normal path, which redraws.

**Risks.**
- Steps 3 and 4 touch the code behind #351 (a render shelved as a PreRender pass) and the
  deferred-AA buffer clobber.
- Step 2 must apply exactly the rules `buildSection()` uses to throw a cache away: 0-page truncated,
  embedded-style fallback, image-header, table-row or CSS degraded. Otherwise an "Indexing" popup
  lands on a page that was shown as pre-rendered. Pull those rules into one predicate that both
  call, as a separate refactor first.

**Next.** Measure on the X3 before deciding. Its turns are waveform-bound, which may change the
trade-off. A runs there again as of 2026-10-08 (see "X3 and the pre-render heap floor").

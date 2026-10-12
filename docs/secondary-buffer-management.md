# Framebuffers, baselines and the secondary buffer

Reference for what each framebuffer holds and when, where each panel controller keeps its "previous
frame", and how to lend or release the secondary buffer safely. Read it before writing code that
borrows or releases the secondary, draws on top of the frame on screen, or adds a grayscale path.

The display facade is `freeink::FreeInkDisplay` in the freeink-sdk submodule
(`libs/display/FreeInkDisplay`). `EInkDisplay.h` is only a compatibility alias
(`using EInkDisplay = freeink::FreeInkDisplay;`). The firmware reaches the facade through `HalDisplay`
(`lib/hal`) and `GfxRenderer`. Open problems in this area are in
[future_work/display-and-refresh.md](future_work/display-and-refresh.md).

---

## 1. Four things called "the previous frame"

They are different objects, and bug reports in this repo have mixed them up more than once.

| Name | What it is | Owner |
|---|---|---|
| **Glass** | The physical particle state, the only truth. After an anti-aliased page it holds grey at every glyph edge. | Physics |
| **Controller baseline** | What the controller diffs a new frame against. | Controller RAM (UltraChip DTM1, SSD1677 RED) or, on the T5S3, LovyanGFX's own panel buffers |
| **Host copy** (`frameBufferActive`) | The frame the host last pushed. | `FreeInkDisplay` |
| **Advisory flags** | Bits that claim the baseline is in sync: the facade's `_redRamSynced`, and each driver's own (`_redRamSynced`, `_oldPlaneValid`, `_needFullClear`, `_inGrayscaleMode`, ...). | The facade and each driver separately |

**The host copy is the controller baseline on one controller only: the SSD1677**, which takes it as
`prev`. Every other driver ignores `prev` and keeps its own baseline. So borrowing, releasing or
reseeding the secondary changes what the host believes is on the panel (which is what overlays,
popups, screenshots and Quick Resume read), and on the SSD1677 also what the next FAST diffs against.

---

## 2. Boards and controllers

| Board | Controller | Driver | Baseline lives in | Host buffer |
|---|---|---|---|---|
| X3 (ESP32-C3) | UC8253; UC8279d on newer units | `Uc8253X3Driver`, `Uc8279Driver` | DTM1 ("old" plane) | 792×528, 52,272 B |
| X4 (ESP32-C3) | SSD1677; UC8179 or UC8279 on newer units | `Ssd1677Driver`, `Uc8179Driver`, `Uc8279X4Driver` | SSD1677: RED RAM, written from `prev`. UltraChip: DTM1 | 800×480, 48,000 B |
| X4 Pro (ESP32-S3) | same three as the X4 | same | same | 800×480, 48,000 B |
| T5S3 (ESP32-S3) | ED047TC2 through LovyanGFX `Panel_EPD` | `LgfxEpdDriver` | LovyanGFX's 4-bpp panel buffer and step buffer | 960×540, 64,800 B |

The controller is found at boot by a display-bus probe (`freeink::applyXteinkDisplayController()`,
called from `HalGPIO::begin()` on the C3 and from `HalDisplay::begin()` on the S3), and
`FreeInkDisplay::selectDriver()` picks the driver from `BoardConfig::ACTIVE.displayController`.
Settings > System Information shows which controller a unit has. Controller overview:
[contributing/eink-controllers.md](contributing/eink-controllers.md). T5S3 panel:
[lilygo-t5s3-display-stack.md](lilygo-t5s3-display-stack.md).

---

## 3. The host buffers

`FreeInkDisplay::begin()` allocates two framebuffer-sized heap blocks (`frameBuffer0`, `frameBuffer1`)
with plain `malloc` (`FREEINK_FB_PSRAM` is off on all four boards). The firmware is always
dual-buffer: it never defines `EINK_DISPLAY_SINGLE_BUFFER_MODE`.

| Pointer | Meaning |
|---|---|
| `frameBuffer` | The write target: `clearScreen()`, glyphs and bitmaps land here. |
| `frameBufferActive` | The secondary. After a full-frame refresh it holds the frame just pushed. |
| `_secondaryLent` | The secondary block while it is lent out (private). |

`displayBuffer()`, `triggerDisplay()` and `triggerDisplayAsync()` end in `swapBuffers()`, so right
after a refresh:

```
frameBufferActive = the frame just pushed (what the panel shows)
frameBuffer       = the frame pushed before it (one generation stale)
```

These do **not** swap:
- `displayGrayBuffer()`: the two-push AA flow relies on `frameBufferActive` still holding the base it
  pushed.
- `displayGrayscaleFrame()`, `triggerGrayscaleFrame()`, `displayGray8Canvas()`: the only driver that
  supports them (LGFX) diffs against its own buffer.
- `displayWindow()`: see "Common mistakes".

`swapBuffers()` does nothing while the secondary is away, so the write buffer then also holds what
was last pushed.

The invariant every compositing consumer relies on:

> **I1:** after a refresh, `frameBufferActive` is the panel content, and
> `syncWriteBufferFromDisplayed()` can restore the write buffer from it.

I1 holds only while the secondary is resident and has not been reseeded since the last full-frame
refresh.

### What else ends up in the write buffer

| Producer | Write buffer afterwards | Recoverable from the secondary? |
|---|---|---|
| Any swapping refresh | The previous frame. | Yes |
| Background-A pre-render (`renderPreRenderPass`) | The **next** page. The pre-render lives in the write buffer, not the secondary. | Yes |
| Grayscale plane pass (`renderGrayscalePlanesSequential` / `Interleaved`) | The LSB, then the MSB plane, until the cleanup at its tail copies the displayed frame back. | Yes if resident. With the secondary away the plane stays. |
| `borrowSecondaryBuffer()` | Untouched: the frame from before the last refresh. No copy of the panel exists anywhere. | No |
| `HalDisplay::releaseSecondaryBuffer()` | The displayed frame, copied in before the free; then free for any use. | No, once overwritten |
| `returnSecondaryBuffer()` / `reallocSecondaryBuffer()` | Untouched, but the **secondary** is now a copy of the write buffer (or white) and reports resident. | No, and it looks as if it were |

### Who relies on I1

- Every `syncWriteBufferFromDisplayed()` caller, including `BaseTheme::drawPopup()` and
  `drawBusyIndicator()` with `overlayDisplayedFrame=true`. `syncWriteBufferFromActive()` underneath is
  gated on `frameBufferActive` and silently does nothing while the secondary is away.
- `ActivityManager::dispatchLightPanelGesture()` and the screenshot path in `main.cpp`: sync, then
  `prepareFramebufferForCapture()`, which lets the reader re-render the current page when the write
  buffer holds a pre-render.
- `SleepActivity::renderOverlaySleepScreen()` and `renderLastScreenSleepScreen()`: they draw onto
  whatever the write buffer holds.
- `saveSleepFrameBuffer()` in `main.cpp`: saves `getFrameBuffer()` after the sleep screen's refresh.

---

## 4. Per controller: baseline, refresh modes, grayscale

### UC8253 (X3)

- **Baseline:** DTM1, rewritten from `fb` in `displayFinish()` after every refresh. `prev` is ignored.
- **FAST:** `_fast` bank, differential against DTM1.
- **HALF:** `_half` bank, WW==BW and WB==BB: every pixel is driven toward its target regardless of
  DTM1. One short pass, not an inverting clear.
- **FULL:** DTM1 ← white, `_full` bank, long timing. FULL also runs for any request while
  `_redRamSynced` is false, a resync is pending or boot full-syncs remain.
- **Grayscale overlay** (reader AA): LSB → DTM1, MSB → DTM2, `gc` nudge bank.
- **Grayscale absolute** (sleep cover, BMP viewer): `beginGrayscale(Absolute)` first pushes a real B/W
  base through `displayGrayscaleBase()`; the planes then go out under a bank that leaves WW and BB
  idle. It can add greys to a base; it cannot remove what the base left.
- **`displayGrayscaleBase()`:** takes a clean base (a real `display()` in the fallback mode, then the
  `_aa_pre_bw_mid` settle) when `cleanBaseNeeded`; otherwise the gentle `_aa_pre_bw_mid` differential
  against DTM1.
- **`_grayOnGlass`:** set by every gray waveform; cleared only by `_full`, `_half` or
  `grayscaleRevert()`, never by a RAM restore; part of `cleanBaseNeeded`. It keeps a sleep cover after
  AA pages from diffing against greys that are still on the glass.
- **`cleanupGrayscaleBuffers(bw)`:** DTM1 = DTM2 = `bw`, `_redRamSynced = true`. With `nullptr`:
  `_redRamSynced = false`, so the next push is FULL.

`_redRamSynced` is false only between `displayGray()` and the cleanup that follows it. In normal
reading every turn is `_fast`, every periodic scrub is `_half`, and `_full` does not run.

### UC8279d (X3, newer units)

- **Baseline:** DTM1, synced in `displayFinish()`. `prev` is ignored.
- **FAST:** DU bank against DTM1, only with `_oldPlaneValid`.
- **HALF and FULL:** GC bank diffed against the real previous frame, not a white seed (a white seed
  left white-to-white pixels undriven, and the previous screen ghosted through).
- **Grayscale:** XTF_AA nudge (overlay) or XTH4 (absolute). Afterwards `_oldPlaneValid = false`; an
  absolute pass also sets `_forceFullSyncNext`.
- **`displayGrayscaleBase()`:** the same two branches as UC8253, but no grey-on-glass state.
- **`cleanupGrayscaleBuffers(bw)`:** both planes ← `bw`, `_oldPlaneValid = true`. With `nullptr`:
  `_oldPlaneValid = false`, `_forceFullSyncNext = true`.

### SSD1677 (X4, X4 Pro)

- **Baseline:** RED RAM, managed by the host. FAST writes BW ← `fb` and RED ← `prev`, or keeps RED
  when `prev` is null. HALF and FULL bypass RED (`CTRL1_BYPASS_RED`) and write both planes: absolute.
- **After a blocking refresh with `prev == nullptr`** the driver rewrites BW and RED from `fb`. The
  async path (`displayStart()`) skips that resync: with `prev == nullptr`, RED keeps what it held until
  something rewrites it.
- **`seedPreviousFrame()`** writes RED with no refresh. It is what `syncRedRamFromFrameBuffer()` reaches.
- **Grayscale overlay:** LSB → BW RAM, MSB → RED RAM, custom LUT. `_inGrayscaleMode` then turns the
  next FAST into HALF unless the cleanup ran.
- **Grayscale absolute** (`_cfg.absoluteGrayscale`): `GrayscaleBase::Combined`. No base push; the
  factory waveform drives every pixel from the two planes, and `_needsGrayClear` promotes the next FAST.
- **`cleanupGrayscaleBuffers(bw)`:** RED ← `bw`, clears `_inGrayscaleMode`. With `nullptr`: nothing,
  so the next FAST becomes HALF.
- The first paint after boot or wake is promoted to HALF (`_needsInitialFull`).

### UC8179 (X4, X4 Pro)

- **Baseline:** DTM1 (OLD), synced in `displayFinish()`. `prev` is ignored.
- **FAST:** DU partial against OLD, only with `_oldPlaneValid && !_needFullClear`.
- **HALF:** charge scrub: OLD ← ~target, GC bank, so every pixel passes through a transition cell.
- **FULL:** OLD ← white, GC bank.
- **Grayscale:** `_grayBase` (a PSRAM snapshot of the B/W base; the allocation fails on the C3) restores
  both planes after `displayGray()`, and `_redriveAfterGray` routes the next FAST through
  `transitionGrayscaleBase()` (XTF_PRE_BW_MID, no flash) instead of a DU.
- **`displayGrayscaleBase()`** treats a Half or Full fallback as a floor: only a Fast fallback may
  become the differential transition.
- **`cleanupGrayscaleBuffers(bw)`:** nothing to do if the planes were already restored from
  `_grayBase`; otherwise both planes ← `bw`. With `nullptr`: `_needFullClear = true`.

### UC8279 800×480 (X4, X4 Pro)

The same model as UC8179. After a grey page `_redriveAfterGray` makes the next FAST seed
OLD ← ~target (a DU scrub). `displayGrayscaleBase()` promotes a Half fallback to a true Full GC.
`cleanupGrayscaleBuffers(bw)` writes DTM1 only; with `nullptr` it sets `_needFullClear`.

### ED047TC2 through LovyanGFX (T5S3)

- **Baseline:** inside LovyanGFX: `Panel_EPD`'s 4-bpp buffer and per-pixel step buffer. The driver's
  8-bit PSRAM canvas is rebuilt from `fb` by `fillCanvasBW()` on every push. `prev` is ignored.
- **FAST:** `epd_fast`, the differential bank. **HALF and FULL:** `epd_text`, the clean bank, after
  `normalizeForCleanBank()` re-tags the screen so that the clean bank scrubs all of it.
- **Reader AA:** single push (`displayGrayFrame()` / `displayGrayFrameStart()`): base and greys in one
  waveform. The two-push overlay (`displayGray()`) darkens the canvas the base left
  (`overlayCanvasGray()`) and refreshes under the base's bank.
- **Sleep cover:** native 8-bit grey through `borrowGray8Canvas()` / `displayGray8Canvas()`; no planes.
- **`cleanupGrayscaleBuffers(bw)`:** `fillCanvasBW(bw)`. The canvas is the next diff source, so a
  wrong `bw` drives wrong pixels one refresh later. With `nullptr`: nothing; the next push rebuilds the
  canvas anyway.
- **Async:** `displayStart()` queues the push and returns; `displayFinish()` waits. The yield inside
  the start is load-bearing; see the T5S3 doc.

### What a grayscale pass leaves behind

After an AA pass on page N the glass is page N plus grey at every AA edge. What each driver believes
after the cleanup:

| Controller | Baseline after cleanup | Remembers the grey? |
|---|---|---|
| UC8253 | DTM1 = DTM2 = B/W page N | Yes: `_grayOnGlass` |
| UC8279d | DTM1 = DTM2 = B/W page N | **No** |
| SSD1677 | RED = B/W page N | **No.** Its sleep cover is still safe: the absolute pass has no base push. |
| UC8179 | both planes = B/W page N, from `_grayBase` | Yes: `_redriveAfterGray`, the next FAST is the XTF_PRE_BW_MID transition |
| UC8279 X4 | DTM1 = B/W page N | Yes: `_redriveAfterGray`, the next FAST seeds OLD = ~target |
| LGFX | canvas = B/W page N | LovyanGFX knows what it pushed; the host canvas does not |

Whether a missing memory shows depends on whether the next push's waveform moves a grey pixel to its
rail. On UC8253 one `_half` clears it and the differential `_aa_pre_bw_mid` does not (X3,
2026-09-23); that is why `_grayOnGlass` forces the clean branch.

### Cross-controller facts

- `prev` is consumed by the SSD1677 only.
- HALF is a different waveform on each controller: single-pass target drive (UC8253), GC diff against
  the real old frame (UC8279d), BYPASS_RED absolute (SSD1677), charge scrub (UC8179), invert-seed
  scrub (UC8279 X4), clean bank (T5S3). A fix tuned on one does not carry over by mode name.
- `cleanupGrayscaleBuffers(nullptr)` means "no baseline, take a clean sync on the next push"
  everywhere, implemented differently: UC8253 drops `_redRamSynced`; UC8279d sets
  `_forceFullSyncNext`; UC8179 and UC8279 X4 set `_needFullClear`; SSD1677 leaves `_inGrayscaleMode`
  set so the next FAST becomes HALF; LGFX rebuilds its canvas on the next push.
- `displayGrayBuffer()` hands every driver the write buffer, which holds the last plane at that point.
  SSD1677, UC8253 and UC8279d ignore it; UC8179 and UC8279 X4 use `_grayBase`; LGFX darkens its canvas.
  `HalDisplay::displayGrayBuffer()` still copies the displayed frame into the write buffer first, on
  LGFX only.
- `GfxRenderer::beginAbsoluteGrayPass()` consumes a pending refresh override (for example the HALF the
  reader arms on exit) and hands it to the driver as the base mode.
- The facade's `_redRamSynced` (`isRedRamSynced()`) is advisory and means something only for the
  SSD1677; it is always false on the X3. The `redSynced=` field in `[FBUF]` log lines is this flag
  and prints `n/a` on the X3. It says nothing about any driver's own flags.

---

## 5. Lending the secondary

There are three ways to get framebuffer memory back. Prefer the first.

| | Borrow (`borrowSecondaryBuffer` / `returnSecondaryBuffer`) | Release (`releaseSecondaryBuffer` / `reallocSecondaryBuffer`) |
|---|---|---|
| The block | stays owned by the display; handed out as scratch (callers wrap it in a `BuildArena`) | freed to the heap |
| Getting it back | cannot fail | realloc can fail on a fragmented heap |
| Write buffer on entry | untouched: the frame from before the last refresh | `HalDisplay` copies the displayed frame in first |
| Pending refresh | drained | drained |
| On return | secondary seeded from the write buffer; one-shot RED handling armed | same |

The third way, releasing **both** buffers, is only for sessions that end in a reboot (Scenario 2).

Borrowers today: Home cover loading, the Library's titles, covers and book-index build
(`FileBrowserActivity`), the font selector's previews (`FontSelectionActivity`), first-open indexing
in `ReaderActivity`, the reader's Background-B and Background-C builds, the blocking build when it can
lend, the reader's image warm and image lane, and the image-header walk. Releasers:
`trimMemoryForNetworkSession()` (network sessions, which reboot), `SerialTransferActivity` under low
heap, `SleepActivity` (cover and custom screens), and the reader's blocking section build when nothing
can be lent or a Background-C build failed. The full list, with the reasons, is in
[memory-allocation-strategy.md](memory-allocation-strategy.md) §9.1.

### Scenario 1 — a temporary lend while the screen stays live

1. Hold the render lock.
2. If you will draw on top of the current screen while the block is out, call
   `syncWriteBufferFromDisplayed()` first. A borrow does not seed the write buffer.
3. On a non-X3 board call `syncRedRamFromFrameBuffer()` while `frameBufferActive` still holds the
   displayed frame. On the SSD1677 this writes it into RED; elsewhere it does nothing.
4. Borrow (or release).
5. If FAST refreshes will happen during the window, call `setSingleBufferFastDiff(true)`. Without it
   every FAST on a non-X3 board becomes HALF.
6. Do the work. B/W refreshes work; anti-aliasing does not (the reader turns AA off while
   `hasSecondaryBuffer()` is false).
7. Return (or realloc), then `setSingleBufferFastDiff(false)`. No reseed is needed: the return arms a
   one-shot (`_redBaselineAuthoritative`), so the next full-frame FAST diffs against the RED plane the
   SSD1677 kept rather than against the unproven seed.

Background-B seeds nothing and opts into nothing: no refresh happens while it holds the block, because
every render takes the block back first (`recoverSecondaryBufferIfNeeded()`).

### Scenario 2 — releasing both buffers before a reboot

`GfxRenderer::releaseFrameBuffers()` frees both buffers; the web server calls it after painting its
QR screen. The panel keeps showing its last image. No display call may follow until the device
reboots; `displayBuffer()` rejects a flush while the pointer is null. `releaseBuffers()` refuses while
the secondary is lent, which is why `trimMemoryForNetworkSession()` returns a lent block first.
`releaseFrameBuffersWithScratch()` installs a caller-owned scratch block as the write target, for the
reader's pre-reboot image warm pass.

Deep sleep: `SleepActivity` releases the secondary for cover and custom screens and never reallocs;
waking resets the chip. Quick Resume saves `getFrameBuffer()` to the SD card after the sleep screen
(`saveSleepFrameBuffer()`); on wake `loadSleepFrameBuffer()` loads it and pushes it with HALF.

### Code patterns

**Pattern 1a — release, keep FAST during the window** (`trimMemoryForNetworkSession()`,
`SerialTransferActivity`):

```cpp
renderer.syncRedRamFromFrameBuffer();  // while the secondary holds the panel
if (renderer.releaseSecondaryBuffer()) renderer.setSingleBufferFastDiff(true);
// ... heap-hungry work; B/W refreshes stay FAST ...
renderer.reallocSecondaryBuffer();           // skip when the session reboots
renderer.setSingleBufferFastDiff(false);
```

`syncRedRamFromFrameBuffer()` is safe on every panel: where a FAST refresh needs no host frame
(`fastRefreshNeedsHostFrame()` false, the X3) there is no RED to seed and it returns at once.

**Pattern 1b — borrow as an arena** (`HomeActivity`, `FileBrowserActivity`, `FontSelectionActivity`,
`ReaderActivity`):

```cpp
renderer.syncWriteBufferFromDisplayed();     // only if you will draw over the current screen
renderer.syncRedRamFromFrameBuffer();
size_t size = 0;
if (uint8_t* block = renderer.borrowSecondaryBuffer(&size)) {
  renderer.setSingleBufferFastDiff(true);
  // ... BuildArena arena(block, size); work ...
  // destroy everything allocated in the arena before the block goes back
  renderer.returnSecondaryBuffer();          // cannot fail
  renderer.setSingleBufferFastDiff(false);
}
```

**Pattern 2 — release both before a reboot** (web server):

```cpp
renderer.displayBuffer();        // the last frame this session shows
renderer.releaseFrameBuffers();  // no display call until the reboot
```

### Refresh-mode downgrade while the secondary is away

`FreeInkDisplay::resolveReleasedMode()` decides what a FAST request means:

| State | A FAST request becomes |
|---|---|
| Secondary resident | FAST |
| Away, opt-in off, X3 | FAST (the baseline is in DTM1) |
| Away, opt-in off, any other board | **HALF** |
| Away, `setSingleBufferFastDiff(true)` | FAST, diffing against what the controller kept |

The rule keys on `PanelSel::X4`, and every board that is not an X3 reads as `X4`, including the
UC8179, UC8279 and T5S3 boards, which keep their own baseline and would not need it. On an X4
(SSD1677) a downgraded FAST took ~1755 ms against ~476 ms (2026-08-19). Ask the display for this
state (`hasSecondaryBuffer()`, `isRedRamSynced()`); do not mirror it in a HAL flag, because after a
failed realloc the copy and the truth diverge.

---

## 6. API reference

On `FreeInkDisplay`, forwarded by `HalDisplay` and `GfxRenderer` unless noted.

| Method | Notes |
|---|---|
| `borrowSecondaryBuffer(size_t*)` / `returnSecondaryBuffer()` | Lend the block without freeing it. Return cannot fail. |
| `releaseSecondaryBuffer()` / `reallocSecondaryBuffer()` | Free and reallocate. Realloc returns false on OOM. `HalDisplay`'s release copies the displayed frame into the write buffer first and logs a double release. |
| `hasSecondaryBuffer()` | `frameBufferActive != nullptr`. False both when lent and when released. |
| `syncWriteBufferFromDisplayed()` (`GfxRenderer`) | Copy the secondary into the write buffer (`syncWriteBufferFromActive()`). No-op while the secondary is away. |
| `syncRedRamFromFrameBuffer()` | Seed the SSD1677's RED from the displayed frame. No-op on the X3 and on drivers without `seedPreviousFrame()`. |
| `setSingleBufferFastDiff(bool)` | Keep FAST while the secondary is away. |
| `cleanupGrayscaleWithPreviousBuffer()` | Tail of every plane pass: hands the driver `frameBufferActive` as the baseline and copies it back into the write buffer. With the secondary away the driver is told there is no baseline. |
| `releaseFrameBuffers()` / `releaseFrameBuffersWithScratch()` (`GfxRenderer`) | Free both buffers. A reboot must follow. |

There is no public way to tell a lent secondary from a released one; `_secondaryLent` is private.

---

## 7. Common mistakes

**Freeing `frameBuffer0` or `frameBuffer1` directly.** `swapBuffers()` changes which slot is the write
target. Always go through the API.

**A FAST refresh with the secondary away and no opt-in.** On every board but the X3 it silently
becomes HALF. Call `setSingleBufferFastDiff(true)` when FAST updates are needed during the window.

**Leaving `setSingleBufferFastDiff` on after the return.** Turn it off once the secondary is back.

**Any display call after `releaseFrameBuffers()`.** It is terminal until the reboot.

**Treating a borrowed and a released secondary as the same state.** `hasSecondaryBuffer()` is false in
both. A release seeds the write buffer with the displayed frame; a borrow does not, so the write buffer
then holds the frame from before the last refresh, and `syncWriteBufferFromDisplayed()` cannot rescue
it. Treat "no secondary" conservatively; `ActivityManager::showBusyIndicator()` skips in both cases for
this reason.

**Expecting a grayscale cleanup to restore the write buffer while the secondary is away.** It cannot:
the plane stays in the write buffer, and the driver takes a clean sync on its next push.

**Assuming `displayWindow()` updates `frameBufferActive`.** It does not swap, so after a windowed
refresh the host copy still holds the old content for that rectangle, and the next FAST can leave
those pixels as the window left them. The SSD1677's window also returns silently when x or width is
not byte-aligned. Nothing calls `GfxRenderer::displayWindow()` today.

**Reading `redSynced=` in `[FBUF]` lines as a driver fact.** It is the facade's SSD1677 flag.

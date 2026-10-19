# Background work in the EPUB reader (A / B / C)

How the reader hides latency behind three cooperative background mechanisms. This is the
**current-state reference**. Code is in `src/activities/reader/EpubReaderActivity.cpp` unless named
otherwise.

For the memory model these mechanisms operate under — the borrow-vs-release distinction in
particular, which is what shapes B's and C's gates — see
[memory-allocation-strategy.md](memory-allocation-strategy.md). For what the borrowed buffer holds
and what lending it does to the display, see
[secondary-buffer-management.md](secondary-buffer-management.md).

## Task model

Two FreeRTOS tasks, serialised by one `RenderLock` (the `renderingMutex`):

- **Render task** (`ActivityManager::renderTaskLoop`) — the *only* task that draws. It is
  notification-driven: it takes the `RenderLock`, calls `Activity::render()`, releases the lock,
  and waits for the next `requestUpdate()`. `render()` dispatches to one pass per call via
  `classifyRenderPass()`, checked in this order: `FinishedBook`, `SectionBuilding`,
  `BufferDisplay`, `PreRender`, `BuildSection`, `Normal`.
- **Loop task** (the main Arduino `loopTask`) — input handling, plus idle-time background work
  via `serviceBackgroundWork()` when no input/page-turn is pending.

`renderContents()` releases the lock *before* the (blocking) e-ink waveform wait, so the loop
task gets the CPU during the ~0.5 s refresh — that window is when background work actually runs.

**Loop-task reads of render-owned state.** `section`, and the metadata cache inside `epub`, belong
to whichever task holds the lock; the render task replaces `section` under it. Loop-task code
therefore reads them only with the lock held. The background steps (`runDeferredGrayscalePass()`,
`stepCurrentSectionBuild()`, `stepBackgroundSectionBuild()`) skip the tick while
`RenderLock::peek()` reports the mutex held, then take it blocking. Code that must never wait uses
`RenderLock(RenderLock::Mode::Try)` and checks `ownsLock()`. A Try answers "is the mutex free", not
"is a pass in flight": it can succeed during the waveform window above.

- `skipLoopDelay()` / `preventAutoSleep()`, polled by `main.cpp` every loop, read the build state
  through `refreshLoopHints()`. It caches the last reading taken under a try-lock and keeps it
  while a pass holds the lock. `skipLoopDelay()` is true while C or B is building,
  `preventAutoSleep()` while C is. A `Blocking` build runs whole inside one locked pass, so the
  hints keep their pre-pass reading through it, and such a build does not hold off auto-sleep.
- `shouldSkipPeriodicUpdate()` (the clock-minute status-bar tick, polled by `ActivityManager::loop()`)
  answers "don't skip" when the lock is busy: at worst one redundant status refresh.
- KOReader auto-sync (`serviceAutoSync()`, `EpubReaderAutoSync.cpp`) skips the tick when the lock
  is busy.

## The three mechanisms at a glance

| | What it hides | Runs on | Secondary buffer | When |
|---|---|---|---|---|
| **A** — next-page pre-render | per-page-turn render compute (~90 ms) | render task (`PreRender` pass) | resident | next page needs no image decode & heap ok |
| **B** — next-section pre-build | the "Indexing…" parse when you cross a chapter | loop task | **borrowed** as a build arena; B waits when there is none to lend | idle, lookahead window, reader settled |
| **C** — current-section build | the freeze when you *land* on an uncached section | loop task (build) + render task (draw only) | **borrowed**; released only when there is none to lend | on entry to an uncached section |

A and B are *look-ahead* for a section that's already on screen. C is for the section you just
navigated to and have nothing to read yet — so it has the highest priority.

## Priority — `serviceBackgroundWork()`

```
runDeferredGrayscalePass();                 // 1. AA of the page just shown (visible quality)
if (pendingGrayscale_.active) return;        //    AA still owed → nothing else runs
if (section && section->hasActiveBuild())    // 2. Background C: build the section you're waiting on
  { stepCurrentSectionBuild(); return; }
stepBackgroundSectionBuild();                // 3. Background A re-arm, the image lane, then Background B
```

Rationale: deferred AA finishes the current page's quality; **C** unblocks reading (you can't
read until it produces pages); **A**, the image lane and **B** are speculative look-ahead that only
matter once the current page/section is settled. Of those, the pictures a few pages ahead come
before a chapter fifty pages away: the image lane runs before B, and B takes the secondary buffer only
once the lane has nothing left to decode (`LaneBeforeBuild.h`, see the image lane below).

---

## A — next-page pre-render

Renders the *next* logical page into the inactive framebuffer so a forward turn is a near-instant
`BufferDisplay` instead of a fresh render.

- **Scheduled** after every page display: `renderContents()` and `renderBufferDisplayPass()` set
  `pendingPreRender` and `requestUpdate()` (while a deferred AA is owed, `runDeferredGrayscalePass()`
  makes the request once the AA has run; see the ordering below). **Re-armed** once per
  `(spine, page)` by `stepBackgroundSectionBuild()` after the deferred-AA frees its memory, and once
  more by the image lane (`warmPageForImageLane()`) when it finishes warming the page after the one
  on screen (the earlier attempts skipped it for its uncached images). Both re-arms key on the page
  actually on screen (`lastRenderedSpineIndex_` / `lastRenderedPageIndex_`), never on `currentPage`:
  a navigation dispatched from `main.cpp` moves `currentPage` before its render runs, and arming
  then would turn that render into a `PreRender` pass that never shows the new page.
- **Runs** as the `PreRender` pass (`renderPreRenderPass`) on the render task.
- **Gate:** free heap ≥ the floor `PreRenderGate.h` derives from what the passes cost: a 21 KB trough
  plus half again the most a pass cost in the book (13 KB until one is measured), and while the page
  drawn last drew TrueType glyphs the store did not hold yet, at least the most a pass with such
  misses cost; an image page only once **every image replays from
  its pixel cache**: `.1bit.pxc` always, plus `.bayer.pxc` wherever AA replays it (AA on and the
  secondary buffer resident, the image lane's `warmGrayscale` rule). The image lane has usually
  written them pages ahead. The pass never decodes — that is seconds, unabortable, under the render
  lock. Image pages are not pre-rendered on a single-push panel (`supportsGrayFrame()`, T5S3): its
  capture rides the glyph blit and records no image greys. A refusal logs `PreRender skipped: ...`
  at DBG.
  No pre-render while B is building through page turns (see B). It never crosses a section
  boundary: a chapter's first page always renders fresh (open, see
  [future_work/display-and-refresh.md](future_work/display-and-refresh.md)).
- **Incomplete draws:** the draw runs under `ImageBlock::PlaceholderOnlyScope`, which counts the
  placeholders it draws: a cache that exists can still fail to replay (a stale one is deleted on
  read, a width mismatch is refused), and the draw then shows a placeholder instead of decoding. A
  page with any placeholder, or with glyphs the font decompressor dropped for lack of memory, is
  marked `incomplete`. It keeps `ready` (the framebuffer does hold it) but is never shown: the turn
  (`pageTurn()`) renders that page fresh.
- **Display:** a forward turn onto the pre-rendered page sets `usePreRenderedBuffer`, and the
  `BufferDisplay` pass (`renderBufferDisplayPass()` → `displayPreRenderedPage()`) draws the live
  status bar over it and flushes it. AA then replays the text planes plus the images' grey levels
  (`renderImagesFromGrayscaleCache`); on a single-push panel the planes the pre-render staged go
  out with the flush instead. It does the image-page bookkeeping a fresh render does: clears
  `pageHasPlaceholders` and arms `pendingHalfRefreshAfterImagePage` when the page has images (see
  the refresh notes under "On X3 specifically").
- **Note on X3:** a page turn is *waveform-bound* (~0.5 s), so A only saves the ~90 ms of
  prewarm+BW compute. Its benefit is modest on X3; the panel, not the CPU, sets page-turn speed.
  With a book's fonts loaded the X3 now clears the floor on every turn (2026-10-19, spec 8.3 of
  [the TTF design](superpowers/specs/2026-10-13-ttf-fonts-design.md)): about 60-75 ms a turn, the
  pass's render, and 0.3-0.9 s on a CJK page.

### Ordering: the deferred AA pass runs before the pre-render

On the deferred-AA path, A is re-armed only *after* the AA pass has freed its memory, because the
planes and the pre-rendered page compete for the same heap. A quick turn inside the AA window
therefore pays a full render. The measured cost and the candidate reordering are in
[future_work/display-and-refresh.md](future_work/display-and-refresh.md) under "Pre-render before the
deferred AA pass".

## Image lane — pixel caches ahead of the reader

Decodes the images of the next few pages into their `.pxc` pixel caches between page turns, so the
turn that reaches them replays a cache instead of running a 1-4 s decode, and A can pre-render
them. `stepImageWarmLocked()`, called from `stepBackgroundSectionBuild()` after A's re-arm and
before B; one page per tick.

- **Window:** the page on screen, then the next `kImageWarmLookahead` (5) pages, continuing into the
  next section's first pages when that section's cache exists (not a CSS-fallback cache, which the
  reader rebuilds on entry). A window found clean is remembered (`imageWarmCleanSpine_` /
  `imageWarmCleanPage_`) until the position moves or B completes a section the window reaches into.
- **Gates:** no C build active; a secondary buffer to lend, or B holding it (below); no queued input
  and no gesture in flight; `IMAGE_LANE_SETTLE_MS` (300 ms) since the last turn, the last page on
  screen, or the last preempted decode. The scheduler already runs nothing while the deferred AA is
  owed.
- **Before B** (`LaneBeforeBuild.h`): the lane reports what it found (`LaneBeforeBuild::Window`), and
  B borrows the buffer only when the lane has looked at the window for the page on screen and found
  nothing to decode (`Clean`), or cannot run at all (`Closed`). While the lane waits for its settle
  or has not looked yet (`Unknown`) B waits too, building through or not. When the lane finds a page
  to decode while B builds in the buffer, B gives the buffer back first (a *yield*,
  `yieldBackgroundBorrowToImages()`): the live build is discarded, its inflated XHTML stays on the
  card, and B starts again once the window is clean. A yield is not one of B's preemptions; B
  allows `LaneBeforeBuild::kMaxYields` (4) per target, shared with the page render's (see B), and
  past them the lane waits for B as it used to. While B holds the buffer the window is looked at as
  its pages will be drawn once the buffer is home, grey caches included.
- **Progress:** B never waits for the lane for long. The window has a few pages, and each one is
  decoded, given up on after `kImageWarmMaxTries`, or, when a press cuts its decode short, decoded by
  the render that shows it, so by a chapter's last page at the latest the lane finds nothing left.
- **Warm:** `warmPageForImageLane()` borrows the secondary buffer as the decoders' scratch and writes
  caches only (`GfxRenderer::ScopedCacheOnlyImageWrites`), so the framebuffer, and a pre-rendered
  page in it, is untouched. It decodes at the offsets the page render uses, because the ordered
  dither baked into a `.pxc` follows screen position. It warms the `.bayer.pxc` too wherever AA
  replays it, and large images whatever the placeholder setting says: once its cache exists an
  image is never a placeholder (`ImageBlock::wouldShowPlaceholder()` checks the cache with a stat
  before it probes the image's size).
- **Preemption and misses:** input aborts the decode (`CooperativeAbort`); the page is retried after
  the next settle. A page that comes back incomplete without input (an unsupported or corrupt
  image, a decode refused for heap) gets `kImageWarmMaxTries` (2) attempts per session.
- **After a warm:** if the page on screen showed those images as placeholders, it is redrawn from
  the cache unless input is waiting. If the warmed page is the one after the page on screen, the
  lane arms A (see A, *Scheduled*).

## B — next-section pre-build (look-ahead)

Builds the **next consecutive** sections' caches during idle so crossing a chapter boundary is a
cache hit, not a blocking parse. `stepBackgroundSectionBuild()`, one bounded slice per idle tick.

- **Lookahead:** a *page budget*, not a spine count — `BG_BUILD_LOOKAHEAD_PAGES` (50) of runway
  ahead of the reading position, counting the current section's unread tail plus the sections
  built so far. Whole spines are built until the budget is covered, so many tiny front-matter
  spines get several built while one big chapter covers it alone. The cursor walks forward as each
  settles and re-anchors on any navigation.
- **State machine:** `Probe` (cache check; a cached spine settles at once) → `WaitHeap` (gates,
  re-checked at most once a second) → `Building` (`BG_BUILD_BUDGET_MS` = 40 ms slices) → `Settled`.
- **Builds only inside the borrowed secondary buffer.** `beginBackgroundBorrow()` lends the block as
  the build's arena: the parse working set, the inflate ring and the CSS index bump-allocate inside
  it. The block never enters the heap, so returning it cannot fail. There is no heap-backed B: with
  no buffer to lend (a C build holds it, or a release never came back) B waits, and C builds the
  section when the reader arrives.
  - **Gates:** the image lane is done with its window (see the image lane, *Before B*); the reader
    has been settled for `BG_BUILD_BORROW_QUIET_MS` (**1.5 s**), measured from
    the last page reaching the screen (`lastPageOnScreenMs_`), not the last turn; no button edge is
    queued (`CooperativeAbort::shouldAbortLongTask()`); free ≥ `BG_BUILD_BORROW_MIN_FREE_HEAP_BYTES`
    (14 KB, plus `BG_BUILD_RESOLVE_EXTRA_HEAP_BYTES`, currently 0, for a spine that still owes the
    footnote resolve); contig ≥ `BG_BUILD_BORROW_MIN_CONTIG_HEAP_BYTES` (12 KB). The floor only has to
    get a build started: once running, a build protects itself (the parser stops below 9 KB and
    leaves a truncated section, CSS lookups degrade, an image header that does not fit is refused),
    and B discards every such result. 1.5 s is sized against a cheap retry (setup + parse ~950 ms on
    an X3).
  - **Cost:** while B holds the block the reader has no AA (`secondaryBufferDegraded_`). B seeds
    nothing and opts into no single-buffer fast differential: every render takes the block back
    first (`recoverSecondaryBufferIfNeeded()`), so no refresh happens while B holds it. For that
    reason a clock-minute status-bar tick waits while B holds the block (`shouldSkipPeriodicUpdate()`
    sets `statusRefreshDeferred_`, and `endBackgroundBorrow()` requests the refresh when the block
    comes back); a page turn still renders at once.
  - **Build-through (X3 only):** on the last `BG_BUILD_THROUGH_PAGES` (3) pages of a chapter, B's
    build of the next chapter starts without the quiet period and keeps the block across page turns.
    Those pages draw in B/W with the normal refresh, without AA or a pre-render. If the reader gets
    to the next chapter first, C adopts the live build. X3 only, because its controller keeps the
    displayed frame as its own baseline; elsewhere the SSD1677's RED would first need seeding from a
    frame the lent buffer may no longer hold.
  - **No decode beside the build:** a page render that finds an image to decode while B keeps the
    buffer (`renderContents()`) has B yield it first, and renders as if B had never held it. It
    used to decode on the heap beside the build (X3, 2026-10-19, anti-aliasing off: a JPEG at
    11,780 B free, a PNG refused its 32 KB ring and drawn as a low-memory box). Past B's yields the
    images are drawn as "indexing" placeholders, and the lane decodes them and redraws the page once
    B is done. With the lane before B these yields are the exception: B builds through only once the
    lane has decoded the pictures of the chapter's last pages.
  - **Preemption:** otherwise a page turn ends the borrow, and `endBackgroundBorrow()` tears the live
    build down *before* handing the block back (the build allocates inside it). The parse is lost,
    but the inflated XHTML from phase (a) stays on the card (`Section::abortSectionBuild()` keys on
    `extractDone`), so the retry is the cheap one. After `BG_BUILD_MAX_PREEMPTIONS` (2) the spine is
    left to C; a next-chapter spine abandoned mid-chapter is retried on the chapter's last pages.
    Neither an overlay opening nor a yield to an image decode counts (`endBackgroundBorrow(false)`):
    they say nothing about whether the parse fits between two turns.
- **Lean CSS resolver:** every build uses the disk-backed resolver (`setLeanResolve`). The hot-rule
  LRU it gives up cannot hit during a build anyway: the parser memoises on `tag|class|id`, so the
  resolver only sees first occurrences.
- **Discards and pauses:** B aborts as soon as a slice starts skipping CSS lookups
  (`Section::activeBuildCssDegraded()`), and discards a finished build that came out truncated,
  CSS-degraded, with footnote previews unresolved, with an image header refused or with a table row
  demoted (`clearCache()`). After such a discard, or a failed build, it pauses until the reader
  enters the next chapter (`backgroundPausedForChapter_`); the foreground or C rebuilds that section
  with more headroom.
- **Adoption:** when you cross into a B section, `buildSection()` adopts `backgroundSection_` — a
  completed build is a cache hit; a still-partial build is finished by C.

## C — current-section build (build-while-you-read)

For the section you just entered that has **no cache** (first open, a jump / TOC / percent target,
a settings/orientation change that invalidated caches, or one B was heap-gated off). Closes the
gap B doesn't cover. The render task **only draws**; the build runs on the loop task.

- **Start:** `buildSection()` detects the cache miss, picks a mode (below), draws the "Indexing"
  popup, kicks off the build so `hasActiveBuild()` is true, and returns — handing off.
- **Build:** `stepCurrentSectionBuild()` on the loop task, 40 ms slices, highest reader-build
  priority. On a newly-written target page it `requestUpdate()`s so the render task draws it.
- **Draw:** the `SectionBuilding` pass (`renderSectionBuildingPass`) shows the requested page from
  the in-progress LUT (`Section::loadPageFromActiveBuild`) or the "Indexing" popup until it's built.
  The page is drawn in BW by `displayBuildPage()` under `ImageBlock::PlaceholderOnlyScope`: an image
  with a pixel cache replays it, any other is drawn as an "indexing" placeholder, so no decode runs
  on the build's heap. The `Normal` render after the build draws the images. Page reads flush the
  writer first so a second handle sees committed bytes.
- **Navigation during the build:** only for an explicit `Page` target — forward advances
  optimistically (shows the page once C reaches it, popup until then), back works, back past page 0
  leaves the chapter. Paragraph/anchor/percent/last-page targets resolve only at completion.
- **Finalize (`Done`):** the on-disk LUT is written, the nav target is resolved (a Page target's
  running position is kept; past-the-end crosses to the next spine), then `READING` resumes and the
  `Normal` pass renders the page with AA.
- **Failure (failed / truncated / css-degraded):** discard, latch `forceBlockingBuildSpine_`, and
  fall back to the **blocking** path (which builds with the buffer released for ~52 KB headroom).

### Build mode — `chooseSectionBuildMode()`

| Mode | When | Buffer |
|---|---|---|
| `IncrementalReleased` | **X3** (always), another board that misses the in-place floors, a spine whose resident build already aborted on low heap, or the buffer is already lent | **borrowed** as the build arena; *released* only when there is none to lend. Restored via `recoverSecondaryBufferIfNeeded()` |
| `IncrementalResident` | a non-X3 board that fits the in-place floors of `heapAllowsInPlaceBuild()` (`IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES` 60 KB / `IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES` 28 KB; a CSS book uses the `IN_PLACE_BUILD_CSS_*` pair, 56 KB / 32 KB; both raised for a large spine's inflate ring) | kept resident |
| `Blocking` | `forceBlockingBuildSpine_` latch, no secondary buffer, or a CSS-fallback rebuild | resident if the in-place floors fit (non-X3), else borrowed; released only for the C-failure latch (to gain the ~52 KB a failed borrowed build lacked) or when nothing can be lent; realloc'd at the end |

The mode name is historical: `IncrementalReleased` *borrows* first and only releases as a fallback,
because the lent block never enters the heap and returning it cannot fail.

Device rationale (see [secondary-buffer-management.md](secondary-buffer-management.md)):

- **X3** keeps the differential baseline in the controller's **DTM1**, so giving up the RAM buffer
  costs no display benefit (fast refresh still works), and building out of it keeps CSS parses
  above the resolve floor. So X3 **never builds resident**: it always takes the buffer, in practice
  by borrowing it. Mid-build draws are plain BW off the DTM1 baseline; AA returns once the buffer
  is back. Device-confirmed on X3, 2026-08-11: `Background-C: building spine 1 incrementally,
  secondary buffer BORROWED`, `CSS RESIDENT in arena`, `cap=52272 highWater=28656 failedAlloc=0`.
- **Other boards** decide by heap: builds that clear the in-place floors keep the buffer resident
  (AA stays live, and on the SSD1677 the fast-refresh baseline is still the host copy). The rest
  take it; C opts into single-buffer fast differential so mid-build draws stay FAST.

Both incremental variants set `secondaryBufferDegraded_`; `recoverSecondaryBufferIfNeeded()` (top
of every `render()`, guarded to skip while a build is active) returns or reallocates the buffer on
the first render after the build ends. `onExit()` restores it too if the reader is left mid-build.

---

## Diagnostics

- **Per-page** (`DBG`): `Page summary: … refresh=<fast|half|full> mode=0x.. renderMs=… …`. The
  refresh mode/byte are captured at the page's own `triggerDisplay` (before the deferred-AA display
  call would overwrite the renderer's live last-mode), so they reflect the page, not the AA pass.
- **A** (`DBG`): `Pre-rendered page N/M in X ms (heap … at start, … lowest, cost …)`, with
  `; incomplete, will not be shown` appended for an incomplete draw (plus an `INF` line for images
  that did not replay, an `ERR` line for dropped glyphs); `PreRender skipped: …` for each refusal;
  `PreRender deferred: AA owed` when the pass waits behind the deferred AA.
- **Image lane** (`INF`): `Image lane: spine N page P warmed in X ms[ -- note] (free=… contig=…)`;
  the note says whether the warm was preempted, incomplete, redrew the page on screen or armed A.
- **Background work** (`INF`, every ~5 s under `DEBUG_BACKGROUND_WORK`): `BG work: A runs=…
  completes=… | B runs=… completes=… | C runs=… completes=… state=<probe|waitheap|building|settled>
  spine=… css=… borrow=… preempt=… | preReady=… buildPct=… free=… contig=…`. A `preempt=` count
  climbing toward `BG_BUILD_MAX_PREEMPTIONS` means B is losing races to page turns.
- **C lifecycle** (`INF`): `Background-C: building spine N incrementally, …` (buffer resident,
  already BORROWED, or the borrow/release outcome), `Background-C spine=N complete: M pages`, and
  `Background-C declined for spine N (…); blocking build` when it cannot start. A borrowed buffer is
  the healthy case; a release means there was none to lend.
- **B borrow** (`INF`): `Background-B: borrowed secondary buffer for spine N …` /
  `Background-B: returned secondary buffer (spine N, build discarded|no build live,
  preemptions=K; heap … at start, … lowest)`. Repeated `build discarded` at `preemptions=2` means B
  keeps losing the race to page turns on that spine and is handing it to C.
  `Background-B: giving the buffer back to the image lane|the page render (spine N, yield K of 4)`
  precedes a discard that is a yield; `Page N: images drawn as placeholders until Background-B's
  build is done` means B had no yields left.
- **Heap gates** (`INF`, off by default — build `-DHEAP_GATE_TRACE=1`): one
  `gate=<name> PASS|REJECT free=…(floor=…) contig=…(floor=…)` line per decision. Several floors
  reject *silently* otherwise, so this is the only way to see which one decided a build's path.
- **Render-task stack** (`ERR`, always on): `Render task stack LOW: N bytes free` if the high-water
  margin drops below 1536 B — the render task runs the deepest chains (build parse + image decode +
  dither) and its stack abuts the heap, so an overflow corrupts the heap.

## On X3 specifically (the common device)

- The borrow is what carries look-ahead here: B builds only inside the lent buffer, so on the X3's
  reading heap every next chapter is either a B cache hit or a C build on arrival. Near the end of a
  chapter, build-through lets B finish the next chapter even while the reader turns pages quickly.
- X3 has no resident build mode at all (`chooseSectionBuildMode` returns `IncrementalReleased`
  unconditionally), so on this device C is *always* the borrow-or-release path and the in-place
  floors never apply.
- Page turns are fast (`refresh=fast`) except the scheduled anti-ghost **half** every
  `refreshFrequencyPages` (default 15) turns, and the **half** owed to the page after an image page
  (`halfRefreshAfterImagePage`, on by default; armed by both the fresh and the pre-rendered display
  path). A redraw of the image page itself (a status-bar tick, an overlay return) leaves that half
  pending and takes the normal FAST refresh: `renderContents()` recognises the redraw by
  `lastRenderedSpineIndex_` / `lastRenderedPageIndex_`, read before it overwrites them. On X3 a
  "fast" request is never silently turned into a half — it's honoured, or escalated to a *full*
  only when the driver's baseline isn't synced (which the clean build/restore paths avoid).

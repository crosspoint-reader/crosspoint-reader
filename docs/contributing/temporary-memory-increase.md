# Temporary Memory Increase Logic

The reader temporarily takes over the secondary framebuffer for work that needs more contiguous memory than normal rendering uses: section builds, image decodes, image-header walks. This document explains how it does that, when it frees memory to the heap instead, and what happens when giving the memory back fails.

The rules behind these choices, and the measured numbers, are in [Memory Allocation Strategy](../memory-allocation-strategy.md). This document describes the mechanics.

## The two display buffers

`GfxRenderer` draws through two framebuffers owned by the display driver (`FreeInkDisplay`), each sized to the running panel (one framebuffer: display width in bytes × display height):

**Primary buffer** (`frameBuffer`): the active draw target. Every render call writes here. It is released only as a last resort before a reboot; any real render after that would crash.

**Secondary buffer** (`frameBufferActive`): the previous-frame snapshot, used for the anti-aliasing (AA) grayscale passes and for fast differential refresh. Taking it away degrades AA and fast refresh until it comes back, but the device stays functional.

## Borrow first, release only when needed

There are two ways to take the secondary buffer away (see [Memory Allocation Strategy §3](../memory-allocation-strategy.md#3-the-two-regions-borrow-versus-release)):

- **Borrow** (`borrowSecondaryBuffer` / `returnSecondaryBuffer`): the display lends the block and keeps owning it. The caller wraps it in a `BuildArena` and allocates inside it. The block never enters the heap, so returning it cannot fail.
- **Release** (`releaseSecondaryBuffer` / `reallocSecondaryBuffer`): the block is freed to the heap. Getting it back is a framebuffer-sized `malloc` that can fail on a fragmented heap.

The reader borrows wherever it can: Background-B and Background-C builds, the blocking build, the per-page image warm, the image lane and the image-header walk ([Memory Allocation Strategy §9.1](../memory-allocation-strategy.md#91-who-borrows-and-who-still-releases)).

## The blocking section build

`EpubReaderActivity::compileSectionCache` runs a section build to completion while an indexing popup is shown:

```
GUI.drawPopup(STR_INDEXING)          // before any buffer change: drawing needs a resident buffer
renderer.dropFontMetadata()          // drop SD font kern/interval tables
resolvePendingImageHeadersFromFramebuffer()   // deferred image headers, walked in the lent buffer
choose one:
  in place (X4 only, heapAllowsInPlaceBuild)   // buffer kept resident
  borrow   (borrowSecondaryBuffer → buildScratch_, setExternalBuildScratch)
  release  (releaseSecondaryBuffer)            // Background-C failure latch, or nothing to lend
section->createSectionFile(...)      // phase (a) inflate to html_<spine>.bin, phase (b) parse
hand back:
  borrow  → returnSecondaryBuffer()  // cannot fail
  release → reallocSecondaryEvictingCaches(), else degraded mode / restart heuristic
renderer.restoreFontMetadata()       // the caller, once the build returns
```

The build releases instead of borrowing in two cases. The first is the Background-C failure latch (`forceBlockingBuildSpine_`): Background-C already built this spine in the borrowed buffer and ran out of heap, so the cure is the extra framebuffer of general heap a release gives. The second is having nothing to lend. An X4 in-place build that fails retries with the buffer released.

There is no image decode after the build. A section needs only image dimensions, so images are decoded when their page is drawn or when the image lane reaches them, and both of those borrow the buffer.

## Heap thresholds

The thresholds are compile-time constants, most of them overridable with a `-D` define. They are re-derived from device traces from time to time, so read the current value and its derivation in the source rather than trusting a number quoted elsewhere.

| Constant | Where | What it gates |
|---|---|---|
| `SCT_EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES`, `SCT_EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES` | `Section.cpp` | a heap-backed build with book CSS; the contig floor adds 8 B per CSS rule plus 8 KB (`Section::heapAllowsEmbeddedStyle`). A build in the borrowed buffer is exempt. |
| `IN_PLACE_BUILD_*` | `EpubReaderActivity.cpp` | building with the secondary buffer kept resident (X4 only) |
| `RESIDENT_BUILD_ABORT_*` | `EpubReaderActivity.cpp` | abandoning a resident Background-C build that is running out of heap |
| `BG_BUILD_BORROW_MIN_FREE_HEAP_BYTES`, `BG_BUILD_BORROW_MIN_CONTIG_HEAP_BYTES` | `EpubReaderActivity.cpp` | starting a Background-B build in the borrowed buffer |
| `PreRenderGate::kTroughBytes`, `kUnmeasuredAllowanceBytes` | `PreRenderGate.h` | pre-rendering the next page, with the costs the passes measured |

When the embedded-style gate refuses, `Section::startBuild` builds the section without CSS (a different cache variant) rather than failing. On a later entry the reader finds the no-CSS variant (`Section::isEmbeddedStyleFallback`) and rebuilds with CSS.

## Degraded mode (`secondaryBufferDegraded_`)

When the reader has to draw pages without the secondary buffer (a build is running in it, or a realloc failed), `secondaryBufferDegraded_` is set:

- text AA is off;
- page turns use `FULL_REFRESH` instead of the normal cadence. The exception is a Background-B build-through on the X3 (the last pages of a chapter, `BG_BUILD_THROUGH_PAGES`), where the controller keeps diffing against its own copy of the frame on screen.

A borrow ends with `returnSecondaryBuffer()`, which cannot fail. After a release the reader recovers on its own: at the start of each render, `recoverSecondaryBufferIfNeeded` calls `reallocSecondaryEvictingCaches` and clears the flag when it succeeds. That function frees what is likely to sit in the hole before each attempt: first the font page slots, then the CSS caches, Background-B's section and the image manifest (reloaded afterwards).

## Fragmentation recovery: the pre-reboot warm pass

If the secondary buffer cannot be restored and the heap looks fragmented rather than full, `maybeRestartForFragmentedHeap` reboots into the reader once.

**Condition:**
```
freeHeap >= framebuffer size + 32 KB   // a framebuffer plus a reading session's working set is free
contigHeap < framebuffer size          // but no block holds one framebuffer
!fragmentationRecoveryRestartAttempted_    // first attempt only
```

The framebuffer size is computed at run time from the panel (`getDisplayWidthBytes() * getDisplayHeight()`). Two of the three callers (after a released build, after a released image warm) pass `contigHeap = 0` on purpose: after decode failures under pressure the heap may be corrupt, and walking the free list there has crashed the device. `recoverSecondaryBufferIfNeeded` passes the real largest block.

**Flow:**

1. Save reading progress, but only in the `READING` phase. During a section build the in-memory position is not yet applied, and saving it would overwrite the correct one in `progress.bin`.
2. Allocate a scratch buffer of one framebuffer.
3. Call `renderer.releaseFrameBuffersWithScratch(scratch, scratchSize)`. It frees both framebuffers and the scaled-glyph cache and points `frameBuffer` at the scratch. Pixel writes from the warm pass land in the scratch and are discarded on reboot.
4. Call `section->warmAllImageCaches(...)` with both framebuffers free. This is the one pass with room for the full progressive JPEG workspace, so a `.pxc` that was written at a coarser scale is decoded again.
5. Leak the scratch buffer on purpose; the reboot is unconditional after this point.
6. Call `trySilentRestartToReaderForHeapRecovery()`, which sets the RTC reboot target and restarts. `heapRecoveryRestartLatch` in RTC memory prevents a second recovery reboot if the first one also fails.

After the reboot the section cache and the `.pxc` image caches are on SD, the heap is unfragmented and the reader renders normally.

## `releaseFrameBuffersWithScratch`

```cpp
bool GfxRenderer::releaseFrameBuffersWithScratch(uint8_t* scratch, size_t scratchSize);
```

Calls `display.releaseBuffers()` (frees `frameBuffer0` and `frameBuffer1`; it does nothing while the secondary is lent), releases the scaled-glyph cache, clears the scratch and sets `frameBuffer = scratch`. After this call rendering "works" in the sense that pixel writes do not crash — they go to the scratch buffer — but nothing meaningful is displayed. The device must reboot before any real display operation.

`scratchSize` must be `>= panelWidthBytes * panelHeight`. The function returns false if it is not.

## Primary vs secondary: why the asymmetry

The secondary buffer can be taken away mid-session because black-and-white rendering only uses `frameBuffer` (primary). The secondary is only needed for the AA pass (read as the previous-frame snapshot) and fast differential refresh. Losing it degrades quality temporarily but never corrupts the display.

The primary buffer cannot be released mid-session — it is the active draw target and `frameBuffer = nullptr` would cause a null-pointer write on the next render call. `releaseFrameBuffersWithScratch` substitutes the scratch to prevent this, but marks the device as pre-reboot only.

# Memory Allocation Strategy (reader path)

This is the memory reference for the reader: for every allocation on the reader path, where it
comes from and when it goes back. The **RULE** statements in §2 and §3 and the invariants in §6
are normative. §8 holds the measured conclusions the rules rest on; device numbers carry the board
and the date they were taken. §9 describes how the reader applies the rules today.

Code comments cite this file by section number (§4, §8.4, §8.4a, §9.2, §9.3, §9.6) and by rule
number ("rule 4"). Keep those numbers when editing; add new sections at the end. The measurement
logs these conclusions came from (this file before October 2026, `docs/memory-audit-2026-09.md`,
`docs/heap-work-handover-2026-08-10.md`) are in git history.

Scope: the ESP32-C3 boards (X3, X4), which have no PSRAM. The ESP32-S3 boards (X4 Pro, T5S3) have
8 MB of PSRAM with `CONFIG_SPIRAM_USE_MALLOC` and a 4 KB `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`
threshold, so large allocations land in PSRAM there and the budget below does not bind. The code
and the rules are shared, so new code must still follow them.

---

## 1. The budget

Boot checkpoints (`Startup[...]` lines in `main.cpp`), X3, 2026-08-09:

| Checkpoint | free | largest block |
|---|---|---|
| `after_hw_init` | 215,300 | 114,676 |
| `after_display_fonts` | 87,432 | 61,428 |
| `after_activity_route` | 68,344 | 42,996 |

Fonts and display init take about 128 KB and never give it back.

The two framebuffers (`frameBuffer0`, `frameBuffer1`) are the largest heap consumers. Each is
sized to the running panel (`FreeInkDisplay::allocFrameBufferStorage`), not to the largest panel
the binary supports:

| Board | Panel | One framebuffer |
|---|---|---|
| X3 | UC8253, 792 × 528 | 52,272 B |
| X4 | SSD1677, 800 × 480 | 48,000 B |

"The framebuffer size" below always means one buffer of the running panel, computed the way the
code computes it: display width in bytes × display height.

While reading, the X3 sits at roughly 30–41 KB free and 17–27 KB contiguous (2026-09-26). §8.1 has
the measured build and reading budget.

Two consequences drive everything else:

1. **Contiguity, not total free heap, is the binding constraint.** A session rarely fails on "out
   of memory". It fails on "cannot get one framebuffer in one piece". The recovery restart
   (`EpubReaderActivity::maybeRestartForFragmentedHeap`) exists only as a defragmenter: it fires
   when free heap is at least the framebuffer size plus 32 KB and still no block holds one
   framebuffer.
2. **There is no compaction and there are no exceptions.** A failed `malloc` returns null. Under
   `-fno-exceptions` a throwing `new`, or a `std::vector` / `std::string` that cannot grow, calls
   `abort()`. Every allocation is a `makeUniqueNoThrow` / `new (std::nothrow)` that is checked, and
   container growth on the build path asks for the block first (`heapHasBlockFor`,
   `ParsedText::addWord`).

---

## 2. Classify every allocation by lifetime

Every allocation on the reader path is exactly one of these:

| Class | Lifetime | Where it must come from |
|---|---|---|
| **A. Permanent** | whole session | heap, allocated once at a stable point, never freed |
| **B. Resident** | one page or one section, survives across `loop()` ticks | heap, bounded and O(1) in book size |
| **C. Phase-scoped** | one phase of one build, strictly nested | an arena |
| **D. Per-item transient** | one image, one line, one paragraph, one row | a scoped arena block, or storage reused across items |

**RULE 1 — Class D never cycles through the heap inside a long pass.** A short-lived allocation
repeated inside a long-running pass is what fragments a heap without compaction. It does not
matter that it is small or that it is freed promptly: freeing it does not undo the fragmentation.

**RULE 2 — Class C and D allocations are LIFO, so a bump arena is enough.** `BuildArena` enforces
it: `reserveBlock()` / `release()` only rewind the newest live block, and an out-of-order release
is refused and counted (`releaseFailures()`). Anything that cannot be expressed as LIFO is
misclassified and is really class B.

---

## 3. The two regions: borrow versus release

`FreeInkDisplay` offers two very different operations on the secondary framebuffer:

- `borrowSecondaryBuffer()` / `returnSecondaryBuffer()`: the block stays owned by the display
  (`_secondaryLent`). It never enters the heap, so nothing else can allocate inside it and
  **giving it back cannot fail**. Only one lend exists at a time (a second borrow returns null),
  and `releaseBuffers()` / `reallocBuffers()` refuse while the block is lent.
- `releaseSecondaryBuffer()` / `reallocSecondaryBuffer()`: the block is freed to the heap. Getting
  it back is an ordinary framebuffer-sized `malloc` that **can and does fail**.

**RULE 3 — Prefer borrow over release. Release only when the heap genuinely needs those bytes as
heap, and say why in a comment.** A borrow is failure-free by construction; a release stakes the
session on winning one framebuffer-sized contiguous block later.

Do not turn a release into a borrow by freeing the block and allocating the same bytes back as a
heap arena. That is the borrow with two allocator round trips added, and it only hopes to get the
same region back.

**RULE 4 — While the secondary buffer is released, nothing may perform repeated allocate/free
cycles.** The released region is the contiguous hole that must be handed back whole. Churn in that
window carves up the block you have promised to give back. The same goes for any allocation that
outlives the window (a task stack, a cache allocated on first use, a path string): it pins the
hole.

The failure this rule was written for (X3, 2026-08-09): the image warm pass ran with the buffer
released, eight decodes each took and returned a 32 KB PNG inflate ring or a 12 KB TJpgDec pool,
and the realloc afterwards found 51,188 contiguous bytes against 52,272 needed. The reader
restarted. That pass now borrows (§9.3).

§9.1 lists who borrows and who still releases.

---

## 4. Where each consumer lands

### Class A — permanent (heap, allocated at a stable point)

- Both framebuffers (`FreeInkDisplay::begin`).
- The font system and display init.
- The scaled-glyph cache (about 4.75 KB, about 9.5 KB for a synthesised body size), allocated at
  reader entry by `GfxRenderer::ensureScaledGlyphCache`, not on the first scaled glyph, so it
  cannot land inside a released hole mid-build. Permanent for the reader's lifetime only: reader
  exit gives it back, with the font page slots, through `GfxRenderer::releaseGlyphCaches`, so the
  screens after a book do not run without those bytes.
- Long-lived task stacks. Create them before anything is released: a stack allocated inside a
  released hole pins it for the session.
- Settings, theme, recent books: small and bounded.

### Class B — resident (heap, O(1) in book size)

- The current `Page` and at most one pre-rendered page. A `Page` costs about 10.5 KB on the X3
  (`Page::deserialize`, 10,504–10,636 B, 2026-08-02).
- `Section` state: the page LUT (4 B a page; read from the file when no block fits, see
  `Section::loadSectionFile`), TOC boundaries, and page-break labels (loaded lazily,
  `Section::ensurePageBreakLabels`). Pages stream to SD and are not resident.
- The CSS selector index on heap-backed builds (8 B a rule, at most 12 KB at `MAX_RULES` = 1500)
  and the bounded hot and negative caches. `reallocSecondaryEvictingCaches` evicts them under
  pressure.
- Font page slots between prewarms (about 9 KB in six blocks on the X3). They are cleared before
  every secondary realloc.
- Image manifest records (12 B each, grown nothrow).
- During a build: the parser's reused `ParsedText` vectors and layout scratch (6–8 KB), given back
  at every slice yield (`ParsedText::releaseLayoutScratch`) because a mid-build page draw can run
  there.

### Class C — phase-scoped (arena)

`Section::runBuildParse` splits a build into two phases whose peaks never overlap:

- **(a) extract**: inflate the spine's XHTML to `html_<spine>.bin`. ZIP read buffer, inflate ring
  (at most 32 KB, sized to the entry by `InflateReader::ringSizeFor`) and an 8 KB grow buffer.
  Skipped when the inflated XHTML is already cached.
- **(b) parse**: SAX state (about 9.8 KB), the 1 KB feed chunk, the CSS ruleset, the page block,
  table row blocks, and the font slots and `Page` of mid-build draws.

Arena instances:

| Instance | Backing | Capacity | Lifetime | Where |
|---|---|---|---|---|
| Build arena, borrowed | lent secondary framebuffer | the framebuffer size | one build | `buildScratch_` in `EpubReaderActivity` (Background-B, Background-C, `compileSectionCache`) |
| Build arena, owned | heap | `SCT_PARSE_ARENA_BYTES` (10 KB) | one build | `Section::BuildState::ownedArena`, only when nothing is lent |
| ZIP-scope arena | heap | `zipArenaBytesFor()`: 1 KB + ring + slack | phase (a) | `BuildState::zipArena`, claimed before setup on owned-arena builds (`Section::startBuild`) |
| Image decode scratch | lent framebuffer | the framebuffer size | one page warm | `image_scratch::ScopedArena` (§9.3) |
| Pre-reboot warm scratch | lent region when idle, else heap | `WARM_PASS_SCRATCH_BYTES` (about 40 KB) on the heap | one pass | `Section::warmAllImageCaches` |
| Image-header walk | lent framebuffer | the framebuffer size | one walk | `resolvePendingImageHeadersFromFramebuffer` |
| Cover and indexing passes | lent framebuffer | the framebuffer size | the pass | `HomeActivity`, `FileBrowserActivity::lendForBackgroundWork`, `ReaderActivity` |
| Library index build | lent framebuffer | the framebuffer size | one build step | `LibraryBuilder::step` (join, publish, author lookup), `LibraryOrder::sortBySeries` |
| Font selector previews | lent framebuffer | the framebuffer size | one preview font | `FontSelectionActivity::lendPreviewArena`, `SdCardFont::useArena` |

In the lent region the CSS ruleset is resident in the arena (`CssParser::setIndexArena`), the SAX
state is taken there (`ChapterHtmlSlimParser::setBuildArena`), and the ZIP ring shares the main
arena. On the owned 10 KB arena all three go to the heap. That difference is why the blocking
build borrows when it can (§9.2). §8.2 shows how full the lent region is at each moment of a
build.

Invariants that hold the arena together; new consumers must not break them:

- Declaration order: the arena outlives everything that allocates in it, and `BuildState` tears
  the parser down before it releases the feed-chunk block.
- `BuildArena::release()` rewinds newest-first only.
- `BuildState::initArena()` resets an external arena, so a build never inherits a cursor.
- An arena scope opened for a mid-build draw closes before the render lock is released: a build
  slice can run during the waveform wait and allocate above it.

### Class D — per-item transient

| Allocation | Repeats per | Size | Comes from today |
|---|---|---|---|
| PNG inflate ring and two scanline buffers | PNG decode | ≤ 32 KB + 2 rows | `image_scratch` arena when installed (`PngStreamDecoder::setScratchArena`), else heap |
| TJpgDec work pool or progressive workspace | JPEG decode | 12 KB / about 24 KB | `image_scratch` first (`JpegWorkPool`), else heap |
| ZIP extract ring for an image | lazy extraction | ≤ 32 KB + buffers | the arena passed to `Epub::extractItemToFile`, else heap |
| `PixelCache` band | cached decode | 2–4 KB for a page image, ≤ 24 KB | heap (raw `malloc`) |
| Ditherer rows, JPEG dither band | mono decode | about 3 KB / ≤ 8 KB | heap |
| `PngStreamDecoder` object | PNG decode | about 3 KB + uzlib state | heap |
| `renderFromCache` read buffer | every replay of a cached image | ≤ 4 KB | heap (raw `malloc`) |
| `ParsedText` word vectors and layout scratch | paragraph | 3–4 KB | reused across paragraphs: no allocation per paragraph |
| A line's word bytes | laid-out line | small | the page block on arena builds, else heap |
| `TextBlock` object and `PageLine` | laid-out line | 88 B + 24 B (host) | heap |
| Table cell objects | grid row | about 160 B a cell (host) | heap (the cells' line bytes are in the arena on arena builds) |

The heap rows are all strictly nested inside one decode, one page or one row, so they fit
`BuildArena`'s LIFO model. They are simply not wired to it yet (§9.5).

---

## 5. Arena versus heap

What a bump arena is for, and what it is not:

- It earns its place on large phase-scoped blocks (inflate ring, CSS ruleset, SAX state, decoder
  work pools) and on blocks scoped to one page or one row (the page block, the table row block):
  their contents die together, which `reserveBlock()` / `release()` expresses exactly.
- It is the wrong tool for a build-lifetime store of small objects. 89 % of build allocations are
  128 B or less (§8.3). An arena that lives for the whole build would only collect them.
- Reuse comes before relocation. The largest churn in a build was vector regrowth, and keeping one
  `ParsedText` across paragraphs removed it with no arena at all (§8.3).
- Never take a large contiguous heap block during a section build. The framebuffer realloc
  threshold is a cliff (§8.4a). Carve the block from the lent region, or do without it.
- Size against the arena's own peaks. Anything held across layout coexists with the 32 KB stage of
  the image-header walk, which leaves 64 B free on the X4 (§8.2). Move a heap block into the arena
  only if it fits beside every peak it coexists with.
- Judge a permanent allocation by where and when it lands, not by its size (§8.5, row 6).

---

## 6. Invariants

1. No `malloc` / `free` pair inside a loop that runs while the secondary buffer is released
   (rule 4).
2. Every class D allocation names the arena it comes from, or has a comment saying why it cannot.
3. Heap gates go on the producing side, sized by what the producer will actually allocate, and
   checked before the bytes are spent. (The table fix moved the check from the end of the table
   to each row as its cells accumulate.)
4. Any new resident (class B) state is O(1) in book size.
5. Prefer borrow to release, and document the reason whenever release is chosen.
6. A refusal that changes the output is latched, never cached silently: the section status byte
   (`kStatusImageHeaderDegraded`, `kStatusTableRowDegraded` and `kStatusCssDegraded` earn the spine
   one rebuild per session; `kStatusSimplified` is deterministic and only logged), or
   `PixelCache::PXC_MAGIC_COARSE` in a `.pxc`.
7. Every fixed capacity has a defined behaviour past it: provably above any input, with the bound
   written next to it; or logged once at ERR and latched (`ChapterHtmlSlimParser::noteCapOverflow`
   sets `kStatusSimplified`); or replaced by a nothrow-growing or SD-backed structure. The build
   side and the load side apply the same cap (`Page::MAX_ELEMENTS`, `Page::MAX_PAGES_PER_SECTION`).
8. A contiguous-block floor carries `LARGEST_FREE_BLOCK_SLACK` (16 B): the allocator reports its
   largest block a few bytes under round numbers.
9. A table that grows across a whole parse is reserved up front from an estimate, and past it grows
   in nothrow chunks rather than by doubling into one block (§9.6).

---

## 7. Measuring memory

**Host.** The host tools run the real section build, so most memory questions can be answered
without a device. Build them with the host suite
([contributing/testing-debugging.md](contributing/testing-debugging.md)).

- `epub_pipeline_dump <book.epub> [cacheDir] --bench [--arena=BYTES]` compiles one book. `--bench`
  adds per-spine time, the whole-run heap peak, an allocation size histogram (`alloc_sizes`) and
  the largest allocation sites (`alloc_site`) to stderr. `--arena=N` lends every build a region of
  N bytes, the way the reader lends the framebuffer: 52272 models the X3, 48000 the X4. Without it
  the run models a build with nothing lent (the owned 10 KB arena, the ring and SAX state on the
  heap). Site names need Linux (`dladdr`); on Windows they print as offsets.
- Run it with `WH_HOST_STDIO_UNBUFFERED=1`. That drops glibc's lazily allocated 4 KB buffer per
  open `FILE*`, which has no device counterpart and once showed up as about 25 KB of peak.
- `epub_build_inventory <book.epub> <cacheDir> <outDir> [--arena=BYTES] [--spines=A,B]
  [--viewport=WxH]` (`test/epub_pipeline/InventoryMain.cpp`) records, per build, every heap site
  with its live bytes at the build's start, heap peak and end, and every arena allocation live at
  the arena's peak and at the heap's peak (`BuildArena` trace hooks, `BUILD_ARENA_TRACE`, host
  only). `inventory_report.py <binary> <outDir>/spine_N.txt` symbolizes the records. Linux only (it
  needs `dlfcn.h` and `execinfo.h`). Calibrate `--viewport` so the host paginates like the device
  first (X3: 400 × 430 gave 183 / 145 pages against the device's 180 / 147, 2026-09-26).
- `HeapPeakRegression` (ctest, `test/epub_pipeline/heap_peak_check.py`) runs the dump on six
  fixtures in both modes and fails when a heap-side peak rises more than 8 KB above
  `heap_peak_baseline.txt`. `UPDATE_HEAP_BASELINE=1` re-baselines, on purpose only.
- Fidelity: the host is 64-bit, so objects holding pointers or strings read 1.3–1.6 × their device
  size; byte buffers are exact. The lent arena's high-water matched the device to 4 bytes
  (*Strange Pictures*: host 45,872, X3 45,876, 2026-09-25).

**Device.** What the serial log already carries:

- The periodic `[MEM]` line: `Free`, `Total`, `Min Free`. `Total` moves when `.bss` grows, which is
  the cheapest regression signal there is.
- `Reader mem[...]` lines (free, contig, block counts) and `Watermark DROP` lines that name the
  interval in which the session minimum fell (`DEBUG_MEMORY_CONSUMPTION`, on in the reader).
- `FBUF` release and realloc lines with the contiguous block. A failed realloc dumps one-shot pin
  forensics (`logHeapPinForensics`): the blocks bounding each free span.
- The build summary: `createSectionFile spine=N arena: cap= highWater= lanes(setup= extract=
  resident= parse=) failedAlloc= zipHW=`. `failedAlloc` is the last refused size: non-zero means
  the arena ran out; zero with `highWater` well under `cap` means a failure was on the heap side.

Compile-time switches, off by default (how to set them:
[contributing/testing-debugging.md](contributing/testing-debugging.md)):

- `SCT_HEAP_TRACE=1`: a per-page heap and arena line during every build (`allocBlk`, `freeBlk`,
  `allocBytes`, arena cursor and high-water).
- `HEAP_GATE_TRACE=1`: every reader heap gate prints its arithmetic.
- `EHP_FORCE_BLOCKING_BUILD`: pins every section to the blocking path, for A/B runs (§8.4a).

---

## 8. Conclusions from measurement

### 8.1 The budget during a build and while reading

X3, *Strange Pictures* (78 progressive JPEGs), firmware of 2026-09-25, before the changes in §9.2:

| Moment | free | contig |
|---|---|---|
| Background-C build start | 47.3 KB | — |
| after setup (ruleset, SAX state, chunk in the arena) | 38.1 KB | — |
| after extraction | 38.0 KB | — |
| minimum during the build | 11.3 KB | 8–12 KB |
| reading, after a borrowed build | avg 35.6, min 31.7 KB | avg 27.0, min 22.5 KB |
| reading, after a released (blocking) build | — | 11.8 KB flat |

The last two rows are the same book on the same device: the released build shaped the heap for
the rest of the session. That is why the blocking build now borrows.

After the image-header probe and the footnote resolver's SAX state moved into the arena and the
page-list anchors were packed (X3, 2026-09-26), a borrowed build of the heaviest chapter (180
pages, 27 images) from 44,960 B free bottomed at 23,912 B: a heap cost of 21.0 KB, down from
32.1 KB. The reading state after a page's AA pass had a median of about 37 KB free. With the
build's spike gone, the session minimum came from rendering: 15,320 B during a page turn's image
decode and 17,948 B during a font prewarm.

### 8.2 The arena by moment

A borrowed build of *Strange Pictures*, host model (`epub_build_inventory`), confirmed on the X3
within a fraction of a percent (2026-09-26). The layout is the same on both boards; only the
capacity differs.

| Moment | In use | Free on X3 | Free on X4 | What fills it |
|---|---:|---:|---:|---|
| setup | 3,888 | 48,384 | 44,112 | CSS ruleset 2,861, feed chunk 1,024 |
| extraction | 45,872 | 6,400 | 2,128 | inflate ring 32,768, grow block 8,192, read buffer 1,024 |
| layout, between image walks | about 14,900 | about 37,300 | about 33,100 | + SAX state 9,832, page block 1–2 KB |
| layout, 16 KB walk stage | about 31,800 | about 20,400 | about 16,200 | + ring 16,384 + read 512 |
| layout, 32 KB walk stage | 47,900–48,016 | about 4,300 | **64** | + ring 32,768 + read 512 |

The arena's high water is the extraction phase. During layout, the phase in which the heap is
scarce, most of the region is idle, except during a 32 KB walk stage, which fills it on the X4.
The heap's peak never coincides with a walk.

### 8.3 What a build allocates

- **Small objects dominate the count.** Host, `test_kerning_ligature`, 2026-08-09: 89 % of 19,863
  allocations were 128 B or less; 57 were 8 KB or more.
- **Words are not the churn.** 92.3 % of words are 15 bytes or less and live in the `std::string`
  small-string buffer; only 588 of 7,676 words allocated.
- **Regrowth was the churn.** Every paragraph regrew `ParsedText`'s word vectors from 16 to 128
  entries, and the layout pass allocated about ten locals per paragraph. Keeping one `ParsedText`
  across paragraphs, reusing the layout scratch, and taking each line's word bytes from a page
  block cut allocations per build from 84,728 to 27,177 (*Strange Pictures*) and from 204,437 to
  67,342 (*moby-dick*), host, 2026-09-25, with byte-identical section files.
- **What is left per line** is the `TextBlock` object and the `PageLine` (88 B and 24 B on the
  64-bit host), both on the heap.

### 8.4 Where contiguity is lost

The question was whether small-object churn or retention collapses the largest free block during a
parse. Block counts tell them apart (`SCT_HEAP_TRACE`): `allocBlk` flat while `freeBlk` rises is
fragmentation; both rising is retention. The answer is churn plus placement, not retention.

- **Gradual loss is churn.** X3, 2026-08-11: contig fell from 40,948 to 15,348 across one 17-page
  parse while free heap oscillated between 20 and 40 KB and `freeBlk` rose from 16 to 35. Removing
  the regrowth (§8.3) took most of it. A later variant that shrank the word vectors at every slice
  yield brought it back, because each slice then regrew them: the lowest block a parse saw fell to
  3,956 B, and keeping their capacity mid-paragraph restored 12,788 B on the same chapter (X3,
  2026-09-26).
- **Cliffs are placement.** A page drawn from the partial build while the build's working set was
  resident split the largest block from 30,708 to 8,692 in one page (X3, 2026-08-11). One draw
  broke down as page load −6,144, font slots −9,216, scaled-glyph cache −5,120. Releasing the slots
  afterwards returned every byte and contig did not move: bytes coming back is not contiguity
  coming back. Allocating the scaled-glyph cache at reader entry and taking the slots from the
  build arena (`FontCacheManager::ScopedSlotArena`) raised post-build contig from 22,516 to
  40,948. The draw's `Page` now comes from the arena too (`Section::loadPageFromActiveBuild`).
- **Tables cost count, not bytes.** X3, 2026-08-22: table pages allocated 130–215 extra blocks and
  contig dropped from 26,612 to 20,468 for the rest of the build, although nothing was retained.
  At about 95 KB free the same tables did no lasting damage: how much a burst of churn costs
  depends on how tight the heap already is.
- **Anything that outlives a release window pins it.** Task stacks, manifest path strings and
  build-time path strings were each found bounding the freed hole (X3, 2026-09-25 and 26).

### 8.4a Table layout, and the A/B harness

Pooling table line storage on the heap (`TextBlockLinePool`) cut a table page's block churn by
about a fifth, but the pool's own 6 KB reservation cost about 4 KB of contig and took it from
53,236 to 49,140, below the 52,272-byte framebuffer: 51 realloc failures against 0, and every later
page turn became a HALF refresh (X3, 2026-08-22). Reverted. Most of a table page's excess is the
per-cell objects (`TableCell::lines`, `TableRow::cells`, the `TextBlock` objects), not word
storage. On arena builds the cells' line bytes now come from a row block in the lent region
(`ChapterHtmlSlimParser::layoutTableRow`); the objects are still on the heap.

The A/B method that measured it: build with `-DSCT_HEAP_TRACE=1 -DEHP_FORCE_BLOCKING_BUILD` so both
arms take the same path, clear the cache between arms, and compare each arm's excess over its own
prose baseline (the baselines differed by 30 blocks between arms). The contig a run starts from
varies more between runs than many of the effects measured, so compare deltas across one step,
not end-to-end values.

### 8.5 Eliminated approaches

Each was tried or measured and disproved. Re-proposing one costs the same time again.

| # | Hypothesis | What killed it |
|---|---|---|
| 1 | Adaptive tone mapping leaks memory | Free heap flat across the pass. |
| 2 | The image warm pass is the fragmenter; run it before the realloc | Reordered on device (X3, 2026-08-09): the section build alone took contig from 53,236 to 25,588 with no image decoded. The failure only moved earlier. Reverted. |
| 3 | Per-word `std::string` is the parse churn | 92.3 % of words fit the small-string buffer. The churn was vector regrowth (§8.3). |
| 4 | Arena-back `ParsedText`'s line-breaking scratch | Bounded (a block splits at about 96 words). Reuse across paragraphs removed its churn without an arena. |
| 5 | Lower the heap gates so the X3 builds more | No gate fired on the X3. The `Epub` object pinning the framebuffer's block was the cause. |
| 6 | A fixed 16 KB font group scratch removes the prewarm churn | Halved contig, 34,804 to 13,300 (X3, 2026-08-10). Allocated lazily mid-session, it pinned the largest free region; its `.bss` fallback slots also cost 3,712 B of heap ceiling. |
| 7 | Prune footnote pass B by fragment existence | 0 pruned: the "markers" were page-number links whose targets exist. |
| 8 | Tighten `isMarkerText` to cut false positives | `test_inline_footnotes.epub` deliberately has a bare `<a href="notes.xhtml#n3">[3]</a>` with no `epub:type`. |
| 9 | Run footnote pass A inside the section build | No whole-book parse exists, and the build consumes the previews: circular. |
| 10 | Move the section LUT into the build arena | It outlives the arena: it becomes the reader's LUT. |
| 11 | Give every page render an arena | An ordinary render has nothing to lend (the secondary buffer is the AA target). Only a mid-build draw can borrow, from the build's region. |
| 12 | Shrink the font groups to cut the inflate transient | Works, but an 8 KB cap costs 8.6 % more font flash (measured by re-deflating every chunk; an earlier +37.7 % came from a model). Bounding the decode ring instead gave a 4 KB peak for +2.1 % (`bf39ecfcd`). |
| 13 | Delete the table byte budget once rows stream | True per table, false per row: eight 9,200-byte cells are about 72 KB in one row. The 12 KB budget moved to the row (`MAX_TABLE_ROW_BUFFER_BYTES`). |
| 14 | Pool table line storage on the heap | §8.4a: less churn, but the reservation pushed contig below the framebuffer. |
| 15 | Skip the font prewarm for pages drawn mid-build | The fallback costs 5,642 µs a glyph against about 1 µs prewarmed (X3, 2026-08-11): the page would draw slower than the popup it replaces. |
| 16 | The image manifest's `ZipFile`, held open for the build, causes the mid-build cliff | 220 B, contig unchanged (X3, 2026-08-11). |

### 8.6 Principles

- **Judge a permanent allocation by placement and timing, not by size.** "Allocated once" is not
  "harmless". Row 6 checked that the size was constant and never checked where it would land.
- **`.bss` growth costs the heap ceiling.** Changing a static array's size moves `Total` in the
  `[MEM]` line. Check it.
- **A cache that records a negative result must prove it looked.** A footnote gather that opened
  zero spines once wrote "this book has no footnotes" as a permanent answer.
- **Verify the premise with a measurement before building on it.** The changes that survived were
  each preceded by a measurement that disproved the first theory.
- **A guard with no fixture is a guess.** A table fallback once shipped strictly worse than the bug
  it replaced, with a green suite, because no fixture reached it. Write the fixture that enters the
  branch first.
- **A green suite only covers what the dump prints.** Check that the pipeline dump can see the
  subsystem you changed; anchors, for example, are covered by `AnchorMapTest` and
  `AnchorPageAccuracyTest`, not by the goldens.
- **A probe that samples between units cannot see a spike inside one.** The per-page and per-chunk
  heap samples missed a 9 KB transient inside one chunk; only the allocator's minimum showed it.

---

## 9. How the reader applies the rules

### 9.1 Who borrows and who still releases

Borrow (the lent region becomes an arena):

- Background-B builds only in the borrowed buffer (`EpubReaderActivity::beginBackgroundBorrow`);
  with nothing to lend, it waits. A B build the reader reaches is handed to Background-C together
  with the borrow, without a return and re-borrow.
- Background-C and the blocking build (`compileSectionCache`), §9.2.
- The per-page image warm in `renderContents`, the image lane (`warmPageForImageLane`) and the
  image-header walk, §9.3.
- First-open indexing (`ReaderActivity`), Home's cover pass and frame cache (`HomeActivity`), and
  the Library's title parses and cover work (`FileBrowserActivity::lendForBackgroundWork`).
- The Library's book-index build (`LibraryBuilder`): every phase but the card walk works in the lent
  region and resets it at each step (`LibraryJoin`, `LibraryPublish`, the author lookups), and an
  author's books are sorted there (`LibraryOrder::sortBySeries`). With nothing to lend, the build is
  abandoned for that visit and tried again on the next.
- The font selector's previews (`FontSelectionActivity::lendPreviewArena`): each uncached preview
  loads its SD font into the lent region (`SdCardFont::useArena`), one block per font, rewound
  after the font is unloaded. With nothing to lend, previews load on the heap.

Release (the block goes back to the heap):

- The blocking build after a failed Background-C build, and any build with nothing to lend (§9.2).
- On the X4, an in-place build that failed retries released.
- The per-page image warm when nothing can be lent.
- Session-ending paths: sleep (`SleepActivity`), the network memory trim (`NetworkMemoryTrim`),
  serial transfer.
- The pre-reboot pass of `maybeRestartForFragmentedHeap`, which frees both framebuffers
  (`GfxRenderer::releaseFrameBuffersWithScratch`). See
  [contributing/temporary-memory-increase.md](contributing/temporary-memory-increase.md).

### 9.2 The blocking build and the release fallback

`compileSectionCache` lends the secondary buffer to the build as its arena. A released build runs
on the owned 10 KB arena, puts the CSS index, the SAX state and the inflate ring on the heap, and
builds out of the very hole it has to hand back. Measured on the X4 (2026-08-11): the owned arena
ran at 9,224 of 10,240 bytes while a borrowed build of the same book used 28,652 of 48,000. On the
X3 the reader read at 11.8 KB contig after a released build and about 27 KB after a borrowed one
(§8.1).

The build is still released in two cases:

- **The Background-C failure latch** (`forceBlockingBuildSpine_`). C already ran borrowed and
  failed on the heap, so borrowing again would repeat it; the extra framebuffer of general heap is
  the cure. `fallbackToReleasedRebuild` also escalates on purpose when a borrowed build came out
  truncated, CSS-degraded or with a table row demoted, so that layout is not cached.
- **Nothing to lend**: no secondary buffer, or one already lent elsewhere.

A released build on the owned arena claims its inflate ring first, before the arena and before
setup (`Section::startBuild`). The released hole is often the only block that size, and setup
allocations made first once left the ring nowhere to go: the chapter came up empty and the pins
kept the framebuffer from coming back (X3, 2026-09-26).

Before tearing a failed borrowed build down, `fallbackToReleasedRebuild` logs the arena's
`highWater`, capacity and `failedAllocSize`, plus free and contig. `failedAlloc != 0`: the arena
ran out and the release is earned. `failedAlloc == 0` with `highWater` well under capacity: the
failure was on the heap side.

### 9.3 `image_scratch` and the borrowed region

`image_scratch` (`ImageBlock.h`) is a process-wide decode arena, installed for the length of a pass
with `image_scratch::ScopedArena`. Three places install it:

- the per-page warm in `EpubReaderActivity::renderContents`, which borrows the secondary buffer and
  falls back to releasing it only when nothing can be lent;
- the image lane (`EpubReaderActivity::warmPageForImageLane`), which borrows it between page turns
  and writes caches only (`GfxRenderer::ScopedCacheOnlyImageWrites`);
- `Section::warmAllImageCaches`, now only the pre-reboot pass, which uses the lent region when no
  build is live and a heap arena of `WARM_PASS_SCRATCH_BYTES` otherwise.

There is no eager decode after a build any more: a section needs only image dimensions, and an
image decodes when its page is drawn or the lane reaches it.

The decoders draw their large blocks from the arena and fall back to the heap: the PNG ring and
scanlines (`PngStreamDecoder::setScratchArena`) and the JPEG work pool or progressive workspace
(`JpegWorkPool`).

The heap gates must discount what the arena serves (`image_scratch::canServe`). Borrowing leaves
the decoder a framebuffer's worth less free heap than releasing did, so gates that still charged
for the ring or the pool refused decodes the heap could serve, and every image skipped its `.pxc`
and decoded again on every visit. `minFreeHeapForJpeg()` drops the pool term and the PNG floor
drops to `PNG_DECODE_HEAP_FLOOR_WITH_ARENA` when the arena can serve them. The gate fix and the
borrow only make sense together. Measured on both panels (2026-08-11): every borrow returned
cleanly, contig stayed flat across the render, and every decode wrote its `.pxc`.

`shouldEnableJpegCache()` charges what caching adds: the band (`PixelCache::bandBytesFor`, 2–4 KB
for a page image), the decode's remaining working set (12 KB) and an 8 KB floor.

### 9.4 Font prewarm transients

- A compressed glyph group streams through a ring sized to its longest back-reference
  (`EpdFontGroup::ringBytes`, at most 4 KB, set by `fontconvert.py`), so the group itself is never
  in memory (`bf39ecfcd`, 2026-08-19). Inflating whole groups had needed up to 32 KB at large
  reader sizes, and capping the group size cost flash (§8.5, row 12).
- The page slots (a buffer and a glyph table per prewarmed font and style, up to
  `FontDecompressor::MAX_PAGE_SLOTS`) are class B between prewarms. During a mid-build draw they
  come from the build arena (`FontCacheManager::ScopedSlotArena`).
- The prewarm's text scan grows in 512-byte steps up to 4 KB per style and asks for each block
  first (`FontCacheManager::recordText`). It used to reserve 4 KB per style up front, and on a
  fragmented heap that reserve aborted the device.
- Still open: the prewarm of an ordinary page render set the session's heap minimum on the X3
  (§8.1). See [future_work/memory-and-parsing.md](future_work/memory-and-parsing.md).

### 9.5 Class D still on the heap

`PixelCache`'s band and `renderFromCache`'s read buffer (both raw `malloc`), the ditherer rows, the
`PngStreamDecoder` object, the per-line `TextBlock` and `PageLine` objects, and the table cell
objects (§4, class D). All are strictly nested, so each could take a scoped arena block. The
per-line and per-cell objects are the larger lever; see
[future_work/memory-and-parsing.md](future_work/memory-and-parsing.md).

### 9.6 Pre-size and chunk the per-page build tables

`st.lut` (4 B a page) and the parser's `paragraphLutPerPage` (8 B a page) grow by one entry per page
for the whole parse. Both reserve `estimatePagesForSpine(inflatedSize)` up front (512 bytes of XHTML
a page, at most 512 pages), so a chapter within the estimate takes one block at setup instead of a
run of allocations interleaved with the parse. Err high: at the old estimate of 1,024 bytes a page,
a chapter of 634 bytes a page doubled both LUTs at page 112 (X3, 2026-09-26).

Past the reserve both grow in chunks (`ChunkedVector`, `lib/Memory`) of at most 1 KB, allocated
nothrow and never copied; the page LUT then moves into the `Section` as it is. As `std::vector`s
they doubled into one block, 8 KB and 4 KB past 512 pages and 16 KB and 8 KB past 1,024, through
the throwing `operator new`: a chapter of a book that is one XHTML file aborted the reader mid-build,
and again at the same page on every reopen (crosspoint-reader PR #3027, a 1,300-page chapter). A
refused chunk stops the build at that page (`ChapterHtmlSlimParser::stopOnFullPageTable`) and the
pages before it are kept as a truncated section, the same exit as a low-heap stop.

What would otherwise grow without bound is not held in memory: anchors stream to a spill file
during the parse (`Section::getAnchorSpillPath`) and are copied into the section file at the end,
and the page-list anchors are packed into one pool reserved at setup
(`ChapterHtmlSlimParser::addExternalPageBreakAnchor`).

The tables a chapter can lose entries from without losing the chapter stay `std` containers and
grow only when the heap holds the block (`heapReserveFor`, `HeapFit.h`): the page-list pool, the
printed-page label record and the resident anchors used when the spill will not open. Past that
their entries are dropped, the section is marked simplified (`kCapPageLabels`,
`kCapAnchorsPerChapter`), and the chapter builds whole.

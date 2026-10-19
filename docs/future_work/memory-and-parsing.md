# Memory and parsing

Status: open items as of 2026-10-07. Collected from the heap-work handover (2026-08), the memory audit (2026-09) and the earlier memory-allocation-strategy.md (removed or trimmed in the 2026-10-07 docs cleanup; in git history).

The rules and measurements these items refer to are in [Memory Allocation Strategy](../memory-allocation-strategy.md).

## Text inside a nested table is dropped

**Open.** `ChapterHtmlSlimParser::consumeText` returns early when `currentTable->depth > 1`, so every word inside a table nested in another table is discarded. The nested-table branch of `ChapterHtmlSlimParser::startElement` calls `degradeTable("nested table")`, and its comment says the inner cells fold into the outer table's stream, but the text never arrives there.

**Why it matters.** It breaks "never drop content", silently, on any book that nests tables. It does not depend on heap state, so it reproduces on the host.

**Where.** The early return in `consumeText`; the nested `<table>` branch in `startElement`; the `depth > 1` `</table>` branch in `endElement`; and every handler that tests `currentTable->depth == 1` (`tr`, `td`/`th`, `caption`, `table`). All of them need auditing, not just the one `return`: a `<tr>` or `<td>` inside the nested table must not open rows or cells in the outer one.

**Tried.** Kept out of the streaming table renderer (PR #126) on purpose, as its own change. Nothing since.

**Next step.** Add a fixture with a nested table to `test/epubs` (a `make_test_*.py` generator, like the others) whose golden shows the inner words, then route `depth > 1` text into the degraded outer table's paragraph stream and walk through each `depth` branch.

## Class and id selectors ignore case

**Open.** `CssParser` lowercases both sides of every class and id lookup: selector keys through `normalizedInto()` when a stylesheet is parsed, class attributes in `forEachNormalizedClassToken()`, and ids in `resolveStyle()` and `backgroundImageFor()`. A rule therefore reaches any element whose class or id differs only in case. XHTML class and id names are case-sensitive, and Kindle, ADE and Readest match them that way.

**Why it matters.** It styles text the book did not style. In *The Anarchy* the paragraph rule `.Dial` (display:block, 0.75em, a hanging indent) also lands on `<span class="dial">`, the speaker name in each dialogue line: the names print at about 56 % size and, since #393 made a `display:block` span open a block, on a line of their own. A survey of 109 books found three that collide (`Dial`/`dial`, `TOC`/`toc`) and none that matches a rule only through the lowercasing.

**Where.** `lib/Epub/Epub/css/CssParser.cpp` (the functions above). `CssGapsIdSelector.CaseNormalized` in `test/css_gaps` pins it for ids today.

**Tried.** PR #394 (branch `fix/css-class-case-sensitive`), stacked on #393 and not merged: only the tag part of a selector is lowercased, the name after `.` or `#` stays as written in the rule cache, the background-picture side file and the lookups; `CaseNormalized` and the `HERO` probe in `test/epub_css/CssParserCacheTest.cpp` become case-sensitive pins; `CSS_CACHE_VERSION` 21 → 22 and `SECTION_FILE_VERSION` 79 → 80.

**Next step.** Rebase the branch onto master. `CSS_CACHE_VERSION` is still 21 there, but `SECTION_FILE_VERSION` is 81, so the section bump becomes 81 → 82. Re-run the corpus comparison, then merge.

## No hint when a chapter was simplified

**Open.** When a fixed capacity changes what a chapter shows (footnotes per page, anchors per chapter, page elements, nesting depth), `ChapterHtmlSlimParser::noteCapOverflow` logs it and the section stores `kStatusSimplified`. The reader only logs "cached simplified" on a cache hit (`EpubReaderActivity`, section load); the reader sees less than the book contains and is not told.

**Why it matters.** The cap is deterministic, so a rebuild cannot help; a hint is the only way to tell the reader. It is a product decision, not a memory one.

**Where.** `Section::isSimplified`; the truncated-chapter hint (`truncatedSectionHintRendersRemaining`, `TRUNCATED_SECTION_HINT_LINE_1/2`) is the model to reuse. Its strings are hard-coded English and would need `tr()` keys.

**Next step.** Decide whether the hint is wanted. If it is, show it the way the truncated hint is shown, with translated strings.

## The image lane waits for Background-B

**Done (2026-10-19, branch `perf/reader-phase-memory`).** The lane runs before B: B borrows the secondary buffer only once the lane has looked at its window and found nothing to decode, and a lane decode or a page render that meets a B build in the buffer has B give it back, at most `LaneBeforeBuild::kMaxYields` (4) times per target and not counted as preemptions. See `src/activities/reader/LaneBeforeBuild.h` and [the image lane](../background-rendering.md#image-lane--pixel-caches-ahead-of-the-reader).

**Measured (X3, Build Through test book, `make_test_font_families.py --build-through`).** Where the conflict happened was build-through, not a mid-chapter B build: with anti-aliasing off B took the buffer on a chapter's last pages 0.6 s after the page appeared, before the lane's settle, and the three picture pages decoded on the heap beside its build (11,780 and 15,672 B free at the JPEGs' lows, the PNG drawn as a low-memory box). With anti-aliasing on, the deferred AA pass delayed B past the lane's settle, so the lane already won. With the lane first, the pictures are decoded in the lent buffer before B starts, B builds through without meeting a decode, and it still finished the next chapter before the reader reached it (the turn onto the next chapter was a cache hit, 1.5 s to a settled screen, as before). A page render that decodes in the buffer B gave back reached 32-34 KB free at its low.

**Still open.** The target page of a chapter jump still decodes in the foreground: Background-C holds the buffer until its build ends, and C is not preempted.

## The font prewarm sets the session's heap minimum

**Open.** Once the section build's spikes were gone, the lowest free heap of an X3 session came from an ordinary page render's font prewarm: free fell from 33,316 to 12,476 bytes inside one prewarm while new glyph groups were decoded, a transient of about 20.8 KB (2026-09-26). A later run measured 17,948 bytes at the prewarm low. The render runs with the secondary buffer resident, so there is no region to lend.

**Why it matters.** The parser aborts a build below 9 KB free, and every other heap gate in the reader is sized against the session's low points.

**Where.** `FontCacheManager::recordText` and `endScanAndPrewarm`; `FontDecompressor::prewarmCache` (the page slots and the group stream ring).

**Tried or ruled out.** A permanent 16 KB group scratch halved contig (eliminated); skipping the prewarm costs 5.6 ms a glyph; an arena has nothing to lend during a normal render. Since the measurement, the text scan no longer reserves 4 KB per style up front (`498425e5f`, `ba2d7d512`), which was up to 16 KB of that transient on a four-style page. Not re-measured on a device.

**Next step.** One X3 session at `-DLOG_LEVEL=3` (for the `Prewarm:` trace lines) with the reader's `Watermark DROP` lines, to see whether the prewarm still sets the minimum and what it allocates. Decide from that.

## Preempted image decodes still lose work in some cases

**Open.** A lane decode that a page turn preempts parks if it is a JPEG: its caches keep their finished rows in `.part` files, a checkpoint (`RenderConfig::checkpointPath`) holds the decoder state, and the next warm resumes it. Three cases still lose work (X3, 2026-09-27): the first warm of an image extracts it from the EPUB first, and that extraction cannot be interrupted (1.6 s for a 514 KB JPEG); a resume that meets input at once parks again without progress (118–265 ms each); and PNG images and the DC-only progressive preview restart from the beginning. After a preemption the lane waits only the normal `IMAGE_LANE_SETTLE_MS` (300 ms).

**Why it matters.** At a normal reading pace the lane lost large images to page turns again and again before parking existed: one page was started six times while the reader turned a page every 2 s.

**Where.** `EpubReaderActivity::stepImageWarmLocked` and `warmPageForImageLane` (`imageWarmPreemptedMs_`; `imageWarmMisses_` counts only failures without input); the park and resume code in `JpegToFramebufferConverter.cpp`; `Epub::extractItemToFile`.

**Tried.** Parking (JPEG only). The other candidate, never built: once a page's decode has been preempted twice, retry it only after a longer 1.5 s pause.

**Next step.** From a device log with parking in place, count re-preemptions per page. If parks without progress dominate, require a longer settle after a resume that parks at once. For the extraction, parking the inflate (a 32 KB window) is the candidate.

## Per-line and per-cell objects are still on the heap

**Open.** Each laid-out line still allocates a `TextBlock` object and a `PageLine` on the heap (88 B and 24 B on the 64-bit host), and each grid cell its own objects (about 160 B a cell, 11–15 KB for a page of grid rows, host). Their word bytes already come from the page block or the row block in the lent region.

**Why it matters.** Block count, not bytes, is what fragments the heap during a build. On an X3 table chapter the block count rose from about 387 to 700–750 on the table pages while the bytes stayed in the arena (2026-09-26).

**Where.** `ParsedText` (the `TextBlock` it creates per line), `ChapterHtmlSlimParser` (the `PageLine` per line, `layoutTableRow`), `Page::elements` (`std::vector<std::unique_ptr<PageElement>>`).

**Tried.** A heap pool for table line storage was reverted: its reservation pushed contig below the framebuffer ([Memory Allocation Strategy §8.4a](../memory-allocation-strategy.md#84a-table-layout-and-the-ab-harness)). Moving the line bytes into the arena is done.

**Next step.** Placement-construct the objects in the page or row block, with an arena-aware deleter behind `Page::elements` (about 30 call sites). Measure allocation counts and `HeapPeakRegression` on the host first, then a table chapter on the X3 with `SCT_HEAP_TRACE=1`. Budget for the image-header walk: on the X4 a 32 KB walk stage leaves 64 B of the lent region free, so objects of a page that holds a late image could push the walk out of the arena.

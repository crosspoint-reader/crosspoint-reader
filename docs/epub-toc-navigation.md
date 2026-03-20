# EPUB TOC Navigation and Chapter Page Counting

This document describes how the reader handles EPUB Table of Contents (TOC) navigation, including fragment anchors, multi-spine chapters, and chapter-relative page counting.

## Background: EPUB spine and TOC structure

An EPUB's **spine** is an ordered list of XHTML files that define reading order. The **TOC** (table of contents) maps chapter names to positions in the spine, optionally with fragment anchors (e.g. `chapter1.xhtml#section-5`).

Three layouts exist in practice:

- **1:1** -- one TOC entry per spine item (most common)
- **Multi-TOC-per-spine** -- multiple TOC entries point into a single spine file using fragment anchors (e.g. Moby Dick from Project Gutenberg packs 3-9 chapters per file)
- **Multi-spine-per-TOC** -- a single TOC entry spans multiple spine files (e.g. a long chapter split across files)

Spine items before the first TOC entry (cover pages) and after the last (appendices, copyright) have no TOC entry of their own.

## BookMetadataCache and TOC-to-spine mapping

`BookMetadataCache` builds the mapping between spine items and TOC entries at epub open time. Key details:

- Each `SpineEntry` has a `tocIndex` field set during cache building. For spines with no matching TOC entry, `tocIndex` inherits the previous spine's value (`lastSpineTocIndex`). This means orphan spines (cover pages, appendices) are treated as continuations of the nearest preceding chapter.
- `getTocIndexForSpineIndex(i)` returns the stored `tocIndex` for spine `i` -- a file seek into BookMetadataCache, not computed on the fly.
- `getTocItem(i)` returns the TOC entry (title, spineIndex, anchor) for TOC index `i` -- also a file seek per call, not cached in memory. This is why hot loops must avoid calling it repeatedly.
- `getSpineIndexForTocIndex(i)` does the reverse lookup (TOC index to spine index).

All of these do file I/O to the BookMetadataCache on every call. Code that queries TOC metadata in a loop should cache the results locally first.

## Section cache file format

The section cache (`.bin`) stores pre-rendered page data for a spine item. The file layout:

```
[header: version, render parameters, pageCount, lutOffset, anchorMapOffset]
[serialized pages...]
[page LUT: array of uint32_t file offsets, one per page]
[anchor map: uint16_t count, then (string, uint16_t) pairs]
```

The header size is defined by `HEADER_SIZE` (a constexpr computed via `sizeof` sum) and validated with a `static_assert`. Three functions read this header independently and must stay in sync:

- `loadSectionFile` -- full section load, reads header + builds TOC boundaries from anchor map
- `readCachedPageCount` -- lightweight check, reads header only to get page count
- `getPageForAnchor` -- seeks directly to anchor map offset from header

When modifying the header layout, bump `SECTION_FILE_VERSION` to invalidate stale caches and update all three read paths plus `writeSectionFileHeader`.

## Anchor-to-page mapping

### Recording anchors during parsing

`ChapterHtmlSlimParser` records every HTML `id` attribute and its corresponding page number into `anchorData` (a flat `std::vector<std::pair<std::string, uint16_t>>`). Recording is deferred via `pendingAnchorId` until `startNewTextBlock()`, after the previous text block is flushed to pages via `makePages()`. This ensures `completedPageCount` reflects the correct page.

For TOC anchors specifically, `startNewTextBlock` also forces a page break before recording, so chapters start on fresh pages rather than mid-page. The parser receives the set of TOC anchor strings via `tocAnchors` (a `std::vector<std::string>`) from `Section::createSectionFile`.

### On-disk format

The anchor data is serialized at the end of the section cache file (`.bin`), after the page LUT. The header stores the anchor map offset. Format:

```
[uint16_t count]
[string anchor_1][uint16_t page_1]
[string anchor_2][uint16_t page_2]
...
```

This data serves two purposes:
- **Footnote navigation** (`getPageForAnchor`): on-demand linear scan for a single anchor
- **TOC boundary resolution** (`buildTocBoundariesFromFile`): scan matching only TOC anchors

### Data structure choices

All anchor storage uses flat vectors, not `std::map` or `std::set`. On the ESP32-C3, each `std::map`/`std::set` node requires its own heap allocation, causing fragmentation. Vectors use a single contiguous allocation. The entry counts are small enough (typically 1-10 TOC anchors per spine, dozens to hundreds of total anchors) that linear scans are faster than tree lookups at these sizes.

## TOC boundaries in Section

When a section is loaded or created, `Section` builds an in-memory `tocBoundaries` vector mapping each TOC entry in that spine to its start page. This is a small vector (1-3 entries typically) that enables O(1) lookups without file I/O.

### Two build paths

**From in-memory anchors** (`buildTocBoundaries`): Called after `createSectionFile` when the parser's anchor vector is still in memory. Iterates TOC entries and does linear scans against the anchor vector.

**From disk** (`buildTocBoundariesFromFile`): Called from `loadSectionFile` when loading a cached section. Caches the small set of TOC anchor strings first (since `getTocItem()` does file I/O to `BookMetadataCache`), then streams through on-disk anchors matching only those, stopping early once all are resolved. Uses a reusable `std::string` buffer to avoid per-entry heap allocation.

The two functions are kept separate because their iteration patterns differ fundamentally: in-memory iterates TOC entries with inner scans of anchors, while the disk path iterates disk entries with inner scans of the small TOC anchor set.

### Early exit optimization

If no TOC entries in the spine have anchors (`unresolvedCount == 0`), both functions return immediately without storing any boundaries. `getTocIndexForPage` falls back to `epub->getTocIndexForSpineIndex`, which gives the correct answer for the common 1:1 case.

### Query methods

- `getTocIndexForPage(page)` -- binary search on sorted `tocBoundaries` to find which chapter a page belongs to
- `getPageForTocIndex(tocIndex)` -- linear scan to find a chapter's start page
- `getPageRangeForTocIndex(tocIndex)` -- returns `[startPage, endPage)` range for a chapter within this spine

All are in-memory, no file I/O.

## Chapter navigation in EpubReaderActivity

### Chapter skip (long-press)

Navigates by TOC index, not spine index. Uses `getTocIndexForPage` to determine the current chapter, then increments or decrements.

- **Same-spine skip**: Resolves the target page via `getPageForTocIndex` entirely in memory
- **Cross-spine skip**: Sets `pendingTocIndex` (a `std::optional<int>`) which is resolved after the target section loads in `render()`
- **Forward past last TOC entry**: Jumps to end-of-book (spine index clamped in `render()`)
- **Backward before first TOC entry**: Jumps to the spine before the current chapter's first spine (clamped to 0 in `render()`)
- **No TOC entry for spine** (`curTocIndex < 0`): Falls back to spine-level skip

### Chapter selector

The chapter selection activity receives `currentTocIndex` (per-page, not per-spine) so it highlights the correct sub-chapter. Returns `ChapterResult` with both `spineIndex` and `std::optional<int> tocIndex`. The reader resolves the page via `getPageForTocIndex` for same-spine navigation or defers via `pendingTocIndex` for cross-spine.

### Footnote navigation

Uses the existing `pendingAnchor` mechanism from the footnote anchor navigation commit (4d222567). `getPageForAnchor` does an on-demand linear scan of the on-disk anchor data. This is separate from TOC boundaries -- it reads all anchors (not just TOC ones) and is only called for footnote jumps.

## Multi-spine chapter page counting

### prepareSection

`prepareSection` combines section loading and chapter cache building into a single pass. When entering a new TOC chapter, it:

1. Determines the contiguous spine range for the chapter
2. For each spine: loads into `section` (current spine) or a stack-allocated temporary (siblings)
3. Loads or builds the section cache, which populates `tocBoundaries`
4. Queries `getPageRangeForTocIndex` to get each spine's contribution to the chapter
5. Aggregates into `chapterPageInfo` (segments with cumulative offsets)

Siblings are fully loaded via `loadSectionFile` (not just `readCachedPageCount`) so that `tocBoundaries` is available for accurate page range computation.

For same-chapter spine transitions (page-turning between spines within a chapter), `prepareSection` skips the walk because `chapterPageInfo` is already populated.

### Spine range determination

The spine range for a TOC chapter is determined by looking at the next TOC entry:

- If the next TOC entry has an **anchor**, it starts mid-spine, so this chapter shares that spine (`lastSpine = nextToc.spineIndex`)
- If the next TOC entry has **no anchor**, it owns that spine exclusively (`lastSpine = nextToc.spineIndex - 1`)
- For the **last TOC entry**, the range is capped to its own spine to exclude end-of-book material (appendices, copyright pages)

### Per-page TOC index update

After section loading, `render()` checks `getTocIndexForPage` on every frame. When the per-page TOC index changes (e.g. page-turning across an anchor boundary within a spine), it recomputes `chapterPageInfo` for just the current spine's range. This is lightweight -- no file I/O, just an in-memory range lookup.

### Status bar

Uses `getChapterRelativePage()` for the page counter (computed from `chapterPageInfo.segments` with cumulative offsets) and `getTocIndexForPage()` for the chapter title.

## Orphan spine handling

Spine items without a TOC entry inherit the previous spine's `tocIndex` in `BookMetadataCache`. This means:

- Pre-TOC spines (cover pages) may have `tocIndex == -1` if they're before any chapter
- Post-TOC spines (appendices, copyright) inherit the last chapter's `tocIndex`

The chapter skip logic guards against `curTocIndex < 0` and falls back to spine-level navigation. The `prepareSection` last-TOC-entry capping prevents post-TOC spines from inflating the last chapter's page count.

## readCachedPageCount

`Section::readCachedPageCount` is a static method that reads just the page count from a section cache file without loading the full section. It validates the version and render parameters (font, margins, etc.) and returns `std::nullopt` if the cache is stale. Used as a quick existence check before deciding whether to build a section cache.

## Implementation pitfalls and edge cases

### Anchor recording timing

The `pendingAnchorId` deferred recording pattern is critical for correctness. Anchors must be recorded *after* `makePages()` flushes the previous text block (so `completedPageCount` reflects the right page) but the TOC page break must happen *before* recording (so the anchor lands on the new page). Both of these happen inside `startNewTextBlock()`. An earlier design used a `recordAnchor` lambda called at various points in `startElement()`, but this had wrong timing for headings and block elements -- `startNewTextBlock` would consume `pendingAnchorId` before `recordAnchor` could force the page break. Moving all page-break logic into `startNewTextBlock` fixed this.

### pendingAnchorId overwrite on consecutive elements

If two elements with `id` attributes appear before any `startNewTextBlock` call (e.g. nested divs), the second `id` overwrites `pendingAnchorId` and the first anchor is never recorded. This is a known limitation inherited from the footnote anchor navigation commit (4d222567) on master. In practice, TOC anchors are on chapter headings which trigger `startNewTextBlock`, so this doesn't affect TOC navigation.

### wordsExtractedInBlock reset on empty block reuse

When `startNewTextBlock` reuses an empty text block (the early-return path), `wordsExtractedInBlock` must be reset to 0. Without this, footnotes in the reused block could be assigned to wrong pages based on stale word counts from a prior block.

### getTocItem() does file I/O

`epub->getTocItem()` reads from `BookMetadataCache` via file seek on every call. This is why `buildTocBoundariesFromFile` caches the TOC anchor strings into a small vector before entering the disk scan loop -- otherwise the inner loop would do file I/O (BookMetadataCache) for every on-disk anchor entry.

### chapterPageInfo invalidation

`chapterPageInfo.tocIndex` must be reset (via `.reset()` on the `std::optional`) in every code path that changes the reading position in a way that could change the chapter: `onExit`, `jumpToPercent`, cache clear, KOReader sync, and orientation change. Missing a reset site causes stale page counts in the status bar.

### Defensive sort on tocBoundaries

`tocBoundaries` is sorted by `startPage` after building. In well-formed EPUBs, entries are already in order (TOC follows document order). The sort is a safety net for malformed EPUBs where TOC entries might be out of document order. With 1-3 entries it has no measurable cost.

### Stack-allocated temporaries for sibling sections

`prepareSection` uses `std::optional<Section>` (stack-allocated) rather than `std::unique_ptr<Section>` (heap-allocated) for temporary sibling sections. On the ESP32-C3's fragmentation-prone heap allocator, avoiding unnecessary dynamic allocations matters.

## Test epub

`scripts/generate_spine_toc_edges_epub.py` generates `test/epubs/test_spine_toc_edges.epub`, a purpose-built epub that exercises all spine/TOC relationship patterns:

| Pattern | Files | TOC entries |
|---------|-------|-------------|
| Multi-TOC-per-spine (anchors) | frontmatter.xhtml | Dedication, Epigraph, Foreword (3 entries, 1 spine) |
| Normal 1:1 | chapter1.xhtml | Chapter 1 (1 entry, 1 spine) |
| Multi-spine-per-TOC | chapter2_part1.xhtml, chapter2_part2.xhtml | Chapter 2 (1 entry, 2 spines) |
| Multi-TOC-per-spine (nested) | chapter3.xhtml | Chapter 3 + 3 sub-sections (4 entries, 1 spine) |
| Spine with no TOC entry | interlude.xhtml | (absent from TOC) |
| Multi-spine-per-TOC | chapter4_part1/2/3.xhtml | Chapter 4 (1 entry, 3 spines) |
| Multi-TOC-per-spine (nested) | chapter5.xhtml | Chapter 5 + 3 sub-sections (4 entries, 1 spine) |
| Multi-TOC-per-spine (tiny entries) | appendix.xhtml | Appendix A-E (5 entries, 1 spine; D and E are deliberately tiny) |
| Mid-file anchor only | backmatter.xhtml | Colophon (TOC points to #colophon mid-file, not file start) |
| Pre-TOC spine | cover.xhtml | (cover image, no TOC entry) |

### Manual test scenarios the epub supports

- **Chapter skip forward/backward through anchored sub-chapters**: Long-press in chapter 3 or 5 to skip between sub-sections within the same spine
- **Chapter skip across multi-spine chapter**: Skip into/out of chapter 2 or 4 to test `pendingTocIndex` cross-spine resolution
- **Chapter skip past end of book**: Skip forward from the appendix to trigger end-of-book screen
- **Chapter skip backward from first chapter**: Skip backward from frontmatter to test boundary clamping
- **Page turn across anchor boundary**: Read through chapter 3 to test per-page TOC index update and status bar title change
- **Chapter selector highlighting**: Open TOC while reading a sub-section of chapter 3 -- selector should highlight the correct sub-section, not "Chapter 3"
- **Orphan spine navigation**: Page-turn into the interlude (no TOC entry) and verify status bar and chapter skip behavior
- **Page count accuracy**: Check status bar page counts for multi-spine chapters (ch2, ch4) show aggregated totals, and for multi-TOC spines (ch3, ch5) show sub-chapter page ranges

## Performance characteristics

- **Per page turn**: All in-memory. `getTocIndexForPage` (binary search on 1-3 entries), `getChapterRelativePage` (linear scan on 1-3 segments), `getTocItem` for title (one file seek to BookMetadataCache -- noted as a future optimization opportunity).
- **Section load**: One file open for the section cache. `buildTocBoundariesFromFile` scans the anchor map for a few TOC entries with early exit. For multi-spine chapters, siblings are loaded in the same pass.
- **Footnote navigation**: One additional file open to scan the anchor map for a single anchor.
- **1:1 TOC-to-spine (common case)**: No overhead. `unresolvedCount == 0`, `tocBoundaries` stays empty, all queries fall back to spine-level methods. `prepareSection` loop runs once for just the current spine.

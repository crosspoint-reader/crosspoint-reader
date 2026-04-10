# EPUB TOC Anchor Navigation

This document describes how the reader handles EPUB Table of Contents (TOC) entries that use fragment anchors to point into spine files, enabling navigation to sub-chapters within a single XHTML file.

## Background: EPUB spine and TOC structure

An EPUB's **spine** is an ordered list of XHTML files that define reading order. The **TOC** (table of contents) maps chapter names to positions in the spine, optionally with fragment anchors (e.g. `chapter1.xhtml#section-5`).

Two layouts are relevant here:

- **1:1** -- one TOC entry per spine item (most common, no anchors needed)
- **Multi-TOC-per-spine** -- multiple TOC entries point into a single spine file using fragment anchors (e.g. Moby Dick from Project Gutenberg packs 3-9 chapters per file)

Spine items before the first TOC entry (cover pages) and after the last (appendices, copyright) have no TOC entry of their own.

## BookMetadataCache and TOC-to-spine mapping

`BookMetadataCache` builds the mapping between spine items and TOC entries at epub open time. Key details:

- Each `SpineEntry` has a `tocIndex` field set during cache building. For spines with no matching TOC entry, `tocIndex` inherits the previous spine's value (`lastSpineTocIndex`). This means orphan spines (cover pages, appendices) are treated as continuations of the nearest preceding chapter.
- `getTocIndexForSpineIndex(i)` returns the stored `tocIndex` for spine `i` -- a file seek into BookMetadataCache, not computed on the fly.
- `getTocItem(i)` returns the TOC entry (title, spineIndex, anchor) for TOC index `i` -- also a file seek per call, not cached in memory. Code that queries TOC metadata in a loop should cache the results locally first.
- `getSpineIndexForTocIndex(i)` does the reverse lookup (TOC index to spine index).

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
- `getPageForAnchor` -- seeks directly to anchor map offset from header
- `writeSectionFileHeader` -- writes the header during cache creation

When modifying the header layout, bump `SECTION_FILE_VERSION` to invalidate stale caches and update all read paths.

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

### Status bar

Uses `getTocIndexForPage()` for the chapter title, so the status bar shows the correct sub-chapter name when reading a multi-TOC-per-spine file.

## Orphan spine handling

Spine items without a TOC entry inherit the previous spine's `tocIndex` in `BookMetadataCache`. This means:

- Pre-TOC spines (cover pages) may have `tocIndex == -1` if they're before any chapter
- Post-TOC spines (appendices, copyright) inherit the last chapter's `tocIndex`

The chapter skip logic guards against `curTocIndex < 0` and falls back to spine-level navigation.

## Implementation pitfalls and edge cases

### Anchor recording timing

The `pendingAnchorId` deferred recording pattern is critical for correctness. Anchors must be recorded *after* `makePages()` flushes the previous text block (so `completedPageCount` reflects the right page) but the TOC page break must happen *before* recording (so the anchor lands on the new page). Both of these happen inside `startNewTextBlock()`. An earlier design used a `recordAnchor` lambda called at various points in `startElement()`, but this had wrong timing for headings and block elements -- `startNewTextBlock` would consume `pendingAnchorId` before `recordAnchor` could force the page break. Moving all page-break logic into `startNewTextBlock` fixed this.

### pendingAnchorId overwrite on consecutive elements

If two elements with `id` attributes appear before any `startNewTextBlock` call (e.g. nested divs), the second `id` overwrites `pendingAnchorId` and the first anchor is never recorded. This is a known limitation inherited from the footnote anchor navigation commit (4d222567) on master. In practice, TOC anchors are on chapter headings which trigger `startNewTextBlock`, so this doesn't affect TOC navigation.

### wordsExtractedInBlock reset on empty block reuse

When `startNewTextBlock` reuses an empty text block (the early-return path), `wordsExtractedInBlock` must be reset to 0. Without this, footnotes in the reused block could be assigned to wrong pages based on stale word counts from a prior block.

### getTocItem() does file I/O

`epub->getTocItem()` reads from `BookMetadataCache` via file seek on every call. This is why `buildTocBoundariesFromFile` caches the TOC anchor strings into a small vector before entering the disk scan loop -- otherwise the inner loop would do file I/O (BookMetadataCache) for every on-disk anchor entry.

### Defensive sort on tocBoundaries

`tocBoundaries` is sorted by `startPage` after building. In well-formed EPUBs, entries are already in order (TOC follows document order). The sort is a safety net for malformed EPUBs where TOC entries might be out of document order. With 1-3 entries it has no measurable cost.

## Test epub

`scripts/generate_spine_toc_edges_epub.py` generates `test/epubs/test_spine_toc_edges.epub`, a purpose-built epub that exercises spine/TOC relationship patterns. See the script header for the full list of edge cases covered.

## Performance characteristics

- **Per page turn**: All in-memory. `getTocIndexForPage` (binary search on 1-3 entries), `getTocItem` for title (one file seek to BookMetadataCache -- noted as a future optimization opportunity).
- **Section load**: One file open for the section cache. `buildTocBoundariesFromFile` scans the anchor map for a few TOC entries with early exit.
- **Footnote navigation**: One additional file open to scan the anchor map for a single anchor.
- **1:1 TOC-to-spine (common case)**: No overhead. `unresolvedCount == 0`, `tocBoundaries` stays empty, all queries fall back to spine-level methods.

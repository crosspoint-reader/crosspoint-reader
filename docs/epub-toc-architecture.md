# EPUB TOC Architecture

Crosspoint Reader supports complex EPUB Table of Contents (TOC) mappings using a unified, lazy-evaluated caching system optimized for the memory-constrained ESP32-C3 microcontroller.

## Core Concepts

### Spines vs. TOC Entries
- **Spine**: A single file (usually HTML/XHTML) inside the EPUB. Represented by `spineIndex`.
- **TOC Entry**: A navigational entry (chapter, section) defined in the EPUB metadata. Represented by `tocIndex`.
- **Anchor**: An HTML ID (e.g. `chapter2.html#section1`) used to jump to a specific location within a spine.

### Layout Topologies
EPUBs can be structured in several ways:
| Topology | Description | Example |
| :--- | :--- | :--- |
| **1:1 Mapping** | One TOC entry per spine item (most common). `spineIndex` maps cleanly to `tocIndex`. | Standard books where each file is a chapter. |
| **Multi-TOC-per-spine** | Multiple TOC entries point into a single spine file using fragment anchors. | e.g. Moby Dick from Project Gutenberg packs 3-9 chapters per file. |
| **Multi-spine-per-TOC** | A single TOC entry spans multiple spine files. | A long chapter split across multiple files. |
| **Orphan Spines** | Spines that do not have a corresponding TOC entry. They inherit the previous spine's `tocIndex`. | Cover pages before the first TOC entry, appendices, copyright. |

## Caching Architecture

Information about the TOC entries are in the `BookMetadataCache`, but accessing the data may require file seek/reads. To avoid repetitive reading, we stick relevant data in the `TocBoundaryCache`, which is lazily computed when first needed. This keeps relevant TOC metadata (for TOC in the current section/spine) like page number and size available in memory.

### Lazy Target Resolution
When a spine is loaded, `TocBoundaryCache` queries its `baseTocIndex` and determines which TOC entries fall into the current spine. However, it does not immediately compute the start pages for these entries.

Instead, the actual target page lookup is deferred lazily until `getTocStartPage` is called. This eliminates eager parsing of the `.bin` anchor map on load. When the cache needs to resolve an anchor, it invokes a callback (`GetPageForAnchorFn`) to let the `Section` perform the lookup, keeping the architecture decoupled.

This extreme laziness is especially critical to avoid performance regressions in edge cases. For example, some EPUBs have spines with hundreds of TOC entries mapping strictly to individual footnotes or annotations. Eagerly resolving all these targets by scanning the anchor map upon spine load would cause noticeable UI freezes (e.g., locking the UI for seconds just by opening the bookmarks menu). By lazily deferring metadata lookup, we avoid O(N*M) string comparison explosions while still retaining the performance benefits of a cache for repeatedly requested boundaries.

### Lazy Chapter Metrics
When the status bar or bookmarks menu requires the chapter length (`getChapterProgress`), the system first checks the `TocEntry` vector.
If the `metrics` optional is empty, it lazily calculates the chapter length. This may involve instantiating temporary `Section` objects to read the page counts of adjacent spines, if the chapter spans multiple spines. The final `offset` and `totalPages` are then saved into the vector.
Because `EpubReaderActivity` keeps its current `Section` alive, turning pages within a chapter will hit the loaded metrics instantly, eliminating file I/O during reading.

### Chapters vs. Footnotes
`TocBoundaryCache` exclusively targets TOC chapter boundaries and caches their resolved pages so UI elements render instantly.

In contrast, `Section::getPageForAnchor` handles random user interactions (like clicking footnote links or index cross-references). We do NOT cache footnote anchors in `TocBoundaryCache` because storing thousands of string anchors in RAM would trigger immediate out-of-memory crashes on the 300KB ESP32-C3. Instead, `getPageForAnchor` uses a zero-allocation string comparison to perform a one-off file scan only when a footnote is actually tapped, short-circuiting the scan upon finding a match.

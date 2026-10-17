#pragma once
#include <ChunkedVector.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"
#include "FontSizeLadder.h"
#include "SpinePageIndex.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;
class CssParser;
class BuildArena;

class Section {
 public:
  // Forward-declared so the by-reference build/hash API below can name it; defined fully below.
  struct BuildParams;

 private:
  // Page byte-offsets, one per page: the build appends one per laid-out page, and a book that is
  // one XHTML file runs to thousands. Chunked rather than a std::vector so that growing it past
  // the build's reserve never asks for one ever-larger block (and aborts when it is not there):
  // chunks of at most 256 entries, 1 KB, each allocated nothrow. 10000 is
  // Page::MAX_PAGES_PER_SECTION (static_assert in Section.cpp).
  using PageLut = ChunkedVector<uint32_t, 16, 256, 10000>;

  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;
  PageLut lut;  // Cached page byte-offsets; loaded once, avoids per-page LUT seek
  // Where the LUT sits in the cache file when `lut` could not be held (no heap block for it on a
  // long single-file book): page loads then read their offset from the file. 0 otherwise.
  uint32_t lutFileOffset_ = 0;
  bool truncatedCache = false;
  bool embeddedStyleFallback = false;
  // Set by the last build when CssParser hit its own low-heap mode mid-parse
  // (lowHeapSkips > 0): some elements were cached without their styles. The cache is
  // usable but visually degraded; background callers discard it so the foreground
  // blocking path (more headroom) rebuilds it clean. Persisted in the status byte
  // (kStatusCssDegraded) and reloaded by loadSectionFile(), so a blocking build that came
  // out degraded -- the one path that used to write the result without looking -- is
  // rebuilt once on a later entry instead of standing for the life of the cache.
  bool cssLowHeapDegraded_ = false;
  // Set by the last build when a table row was emitted as paragraphs instead of a grid
  // (ChapterHtmlSlimParser::tableRowDegraded). Same persistence and rebuild policy as the
  // CSS flag: the pages are usable, the layout is what the heap allowed.
  bool tableRowDegraded_ = false;
  // Set by the last build when a fixed-capacity limit changed its output (ChapterHtmlSlimParser::
  // capOverflowFlags, or a page past Page::MAX_ELEMENTS). Deterministic -- a rebuild would hit
  // the same limit -- so it is persisted (kStatusSimplified) for the reader to say so, not to
  // rebuild on.
  bool simplified_ = false;
  // Set by the last build when its inline-footnote resolve pass could not complete (OOM, an
  // unreadable note document). The pages are cached under a "previews on" property hash — see
  // EpubReaderActivity::makeSectionBuildParams — but the notes this spine points at never made
  // it into the store, so those markers stay plain and nothing would ever rebuild them.
  // Background callers discard such a result and leave the spine to the foreground path.
  bool footnotePreviewsUnresolved_ = false;
  // Set by the last build when an image was dropped to alt text because the heap gate refused
  // its header read. The pages are cached under the same property hash as a complete build, so
  // the missing image would otherwise be permanent — background callers discard instead.
  bool imageHeaderDegraded_ = false;

  // Records this spine's page count in the book's SpinePageIndex under `requestedHash`. No-op for
  // a truncated or empty section.
  void recordPageCount(uint32_t requestedHash) const;
  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, bool bionicReadingEnabled, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

  struct TocBoundary {
    int tocIndex = 0;
    uint16_t startPage = 0;
  };
  std::vector<TocBoundary> tocBoundaries;
  // Loaded lazily after a build (see pageBreakLabelsPending_), hence mutable: the const label
  // queries are what trigger the load.
  mutable std::vector<std::pair<uint16_t, std::string>> pageBreakLabels;
  // Set by a build's finalize instead of copying the parser's labels into pageBreakLabels.
  // Finalize runs while a released build still has the secondary framebuffer's hole open, and a
  // session-lifetime block taken there splits it: 128 x 28 B = 3584 B landed mid-hole on an
  // illustrated book (one pagebreak per printed page), leaving 36 KB contiguous where the
  // framebuffer needs 52 KB, so it could never come back (X3, 2026-09-25). The first label query
  // runs at render time, after the reader has re-taken the framebuffer.
  mutable bool pageBreakLabelsPending_ = false;

  void buildTocBoundariesFromFile(FsFile& f);
  void buildPageBreakLabelsFromFile(FsFile& f) const;
  void ensurePageBreakLabels() const;

  // Live state of an in-progress section build, shared by the blocking path and the
  // sliceable stepSectionBuild() path. Holds exactly the locals that must survive across
  // build phases (and, for the sliced path, across loop ticks). Declared here but defined
  // in Section.cpp so the heavy parser type stays out of this header.
  struct BuildState;
  // In-flight incremental build, owned across stepSectionBuild() calls. Null when no build
  // is live. Heap-owned so the visitor's &lut capture stays stable across ticks.
  std::unique_ptr<BuildState> buildState_;
  // The live build's arena (nullptr when no build is live, or it has none): read by the per-page
  // heap trace, which sits above BuildState's definition in Section.cpp.
  const BuildArena* activeBuildArena() const;
  // See setExternalBuildScratch. Not owned; must outlive any active build.
  BuildArena* externalScratch_ = nullptr;
  // Outcome of one phase method. Mostly maps to BuildStep: More means the phase yielded
  // mid-way after spending its time budget; RetryNoCss asks the entry function to tear the
  // state down and restart from setup with embeddedStyle=false.
  enum class BuildPhaseResult : uint8_t { Ok, More, Done, Failed, RetryNoCss };
  // Phase methods. Each operates on the shared BuildState and is purely linear (the
  // CSS/heap fallback recursion lives in the entry function, not here), which is what lets
  // them be called either back-to-back (blocking) or with Parse re-entered across ticks.
  BuildPhaseResult runBuildSetup(BuildState& st);
  // Resolves the spine's ZIP stat (inflated size) into the build state once; setup and the
  // parse's EntryReader::open reuse it. False when the entry cannot be sized at all.
  bool resolveSpineStat(BuildState& st);
  // True when the book-keyed inflated-XHTML cache for this spine exists with the expected size,
  // i.e. runBuildParse will feed the parser from it and never needs an inflate ring.
  bool htmlCacheReusable(const BuildState& st) const;
  // Feeds the chapter XHTML to the parser. budgetMs == 0 (blocking path): streams the
  // ZIP entry directly and consumes it in one call. budgetMs != 0 (sliced path): runs in
  // two budget-sliced phases — (a) inflate the entry to a temp SD file, release all ZIP
  // state, then (b) feed the parser from that file — so the inflate ring and the layout
  // working set are never live at the same time. Returns More when over budget, with the
  // live state retained in BuildState for the next call.
  BuildPhaseResult runBuildParse(BuildState& st, uint32_t budgetMs);
  BuildPhaseResult runBuildFinalize(BuildState& st);
  // Runs between the parse phases, once the spine's inflated XHTML is on SD and no ZIP state is
  // live: makes sure every note this spine references has its text in the book's preview store,
  // then points the visitor at it. Never fails the build — see the definition.
  bool beginInlineFootnotePreviewResolve(BuildState& st);
  void finishInlineFootnotePreviewResolve(BuildState& st);

  // Open the section file and seek to the first paragraph LUT entry, validating the header
  // and LUT bounds against fileSize. On success, returns true with `outLutStart` set to the
  // byte offset of the first entry (just past the count) and `outCount` to the entry count.
  // Caller is responsible for closing `outFile`. Returns false on any I/O or validation error.
  bool readParagraphLutHeader(FsFile& outFile, uint16_t& outCount, uint32_t& outLutStart) const;

  // Calculates a stable hash for the render properties in a BuildParams (the cache-variant key).
  // Used to suffix cache files so multiple variants can coexist safely without constant recompilation.
  static uint32_t calculatePropertyHash(const BuildParams& p);

  // Computes the active file path for this section based on rendering properties
  std::string getSectionFilePath(uint32_t propertyHash) const;
  // Path of the book-keyed unzipped-HTML cache for this spine: the inflated XHTML keyed on the
  // spine alone (NOT on render properties), so it is reused across settings changes and rebuilds
  // to skip ZIP inflation. Adapted from crosspoint-reader PR #2452 by GitHub user itsthisjustin.
  std::string getSectionHtmlCachePath() const;
  // Path of the per-build anchor spill: where the parser streams the anchor map as it finds it,
  // instead of holding every anchor in RAM until finalize. See
  // ChapterHtmlSlimParser::setAnchorSpillPath for why.
  std::string getAnchorSpillPath() const;
  // Appends the anchor spill at `spillPath` to the open section file `out`, verbatim.
  static bool copyAnchorSpill(FsFile& out, const std::string& spillPath);
  // Computes the image base path for extract images related to this specific section variant
  // Directory prefix for extracted source images. Content-keyed by the caller (see
  // ChapterHtmlSlimParser), so one extraction serves every layout variant.
  std::string getImageBasePath() const;
  // Garbage collection: Keep only the most recent N variants per chapter
  void evictOldVariants() const;

 public:
  // Naming convention behind getSectionHtmlCachePath(), exposed so code that does not own a
  // Section can find a spine's inflated XHTML — FootnotePreviews reads it instead of
  // re-inflating the ZIP entry. Says nothing about whether the file exists or is complete;
  // callers must validate its size against the spine's inflated size, as the builder does.
  static std::string sectionHtmlCachePath(const std::string& bookCachePath, int spineIndex);

  // The page counts recorded for spines [first, last] under `p` (see SpinePageIndex), so the
  // reader can count a chapter split over several spine items without opening their caches.
  static SpinePageIndex::Totals indexedPageTotals(const std::string& bookCachePath, const BuildParams& p,
                                                  int spineCount, int first, int last, int current);

  uint16_t pageCount = 0;
  int currentPage = 0;

  // Render parameters that determine a section-cache variant. Bundles the long argument
  // list shared by loadSectionFile / createSectionFile / stepSectionBuild so a resumable
  // build can carry them across slices without re-passing ten arguments each call.
  struct BuildParams {
    int fontId = 0;
    float lineCompression = 1.0f;
    bool extraParagraphSpacing = false;
    uint8_t paragraphAlignment = 0;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    bool hyphenationEnabled = false;
    bool fontSizeNormalization = true;
    bool embeddedStyle = false;
    bool bionicReadingEnabled = false;
    bool inlineFootnotePreviews = false;
    uint8_t imageRendering = 0;
    // Sibling-size ladder derived from the body font (see FontSizeLadder). Not part of the
    // property hash: it is a deterministic function of fontId, which is already hashed.
    // Default (empty) = scale-only fallback, the right behavior for SD fonts.
    FontSizeLadder fontSizeLadder;
  };

  // Progress of an incremental (sliceable) section build. Setup/Parse/Finalize are the
  // three temporal phases of createSectionFile; More means the current phase yielded
  // mid-way after spending its time budget and should be resumed on the next call.
  // Done/Failed are terminal. The callers are described in docs/background-rendering.md.
  enum class BuildStep : uint8_t { Setup, Parse, Finalize, More, Done, Failed };

  // Ctor/dtor defined in Section.cpp: both need the complete BuildState type (the
  // unique_ptr member's deleter is instantiated in each), and the dtor must abort a still
  // in-flight incremental build so its partially written cache file doesn't survive.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(const BuildParams& p);
  bool clearCache();
  bool createSectionFile(const BuildParams& p, const std::function<void(int)>& progressFn, bool skipEviction);

  // Incremental section-cache build. Advances the build by at most ~budgetMs of work
  // (budgetMs == 0 means no budget: run to a terminal state in one call) and returns More
  // when work remains. The caller invokes it repeatedly (typically from idle time) until
  // Done or Failed; the build state is owned by this Section across calls. Yield
  // granularity is one ~1 KB inflate-feed chunk, so a slice never interrupts a page's SD
  // write and overshoots the budget by at most one chunk's layout work.
  //
  // Re-entry contract: a later call whose params hash differs from the original request
  // discards the partial build (the partial cache is for the wrong variant) and starts
  // fresh. progressFn/skipEviction only apply when a fresh build is started.
  BuildStep stepSectionBuild(const BuildParams& params, uint32_t budgetMs,
                             const std::function<void(int)>& progressFn = {}, bool skipEviction = false);
  // True while an incremental build is in flight (stepSectionBuild returned More).
  bool hasActiveBuild() const { return static_cast<bool>(buildState_); }
  // Optional external scratch region for the build's arena (arena builds only;
  // ignored otherwise): typically the borrowed secondary framebuffer, so build
  // scratch never touches — or fragments — the heap. The region must stay valid
  // until the build completes or is aborted; it is reset (not freed) per build.
  // Call before the first stepSectionBuild/createSectionFile of a build; a null
  // pointer reverts to the internal heap-backed arena.
  // Not a plain setter: if a build is already live on the small owned arena when a large
  // external region arrives, it restarts the build so the region is actually used. See the
  // definition in Section.cpp.
  void setExternalBuildScratch(BuildArena* scratch);
  // Percent of the spine XHTML consumed by the in-flight build (0–100; 100 once the
  // stream is exhausted and only Finalize remains). 0 when no build is live. Feeds the
  // DEBUG_BACKGROUND_WORK overlay.
  int activeBuildPercent() const;
  // Discard an in-flight incremental build: deletes the partially written cache file
  // (its header was never patched, so it must not be left for loadSectionFile to find)
  // and drops the live parser/inflate state. No-op when no build is live. Call on
  // non-consecutive navigation or when render params change.
  void abortSectionBuild();
  // True when current heap headroom permits an embedded-CSS build (the gate startBuild
  // applies before silently downgrading to no-CSS). Exposed so a background caller can
  // refuse to start instead — a downgraded background build would produce the fallback
  // variant and the foreground would still rebuild with CSS, wasting the work.
  // cssRuleCount sizes the dominant contiguous allocation (the selector index,
  // ~16 B/rule) — pass CssParser::ruleCount(), or 0 when unknown. Never logs; callers
  // log refusals at whatever cadence suits them.
  // arenaBacked: the build will bump-allocate the ruleset from a build arena (the borrowed
  // secondary framebuffer) instead of the heap, in which case no heap floor applies. Reader-side
  // callers gate heap-RESIDENT builds and keep the default.
  static bool heapAllowsEmbeddedStyle(size_t cssRuleCount, bool arenaBacked = false);
  std::unique_ptr<Page> loadPageFromSectionFile();
  // Number of pages fully written to the section file during an active build.
  // Increases monotonically as the build progresses; 0 when no build is live.
  // Pages [0, activeBuildPageCount()) are safe to read via loadPageFromActiveBuild().
  uint16_t activeBuildPageCount() const;
  // Where a navigation target lands in the build in progress, once known: a TOC entry of this
  // spine without a fragment is page 0 immediately; one with a fragment, or a bare anchor, is
  // answered as soon as the parser has recorded it (ChapterHtmlSlimParser::
  // lookupAnchorInActiveBuild). nullopt while unknown or when no build is live. The answer is
  // the same one the finished cache gives (tocBoundaries / the anchor map are built from the
  // same records), so the reader can turn the target into a page target and draw it mid-build.
  std::optional<uint16_t> activeBuildPageForTocIndex(int tocIndex);
  std::optional<uint16_t> activeBuildPageForAnchor(const std::string& anchor);
  // Best-known total page count: the exact pageCount when no build is live (finalized) or once
  // the stream is consumed, otherwise a byte-based projection (pages so far scaled by the
  // consumed fraction) so a "page X of ~Y" display doesn't read off the small build watermark.
  // Never reports fewer than the pages already built. Adapted from crosspoint-reader PR #2452 by
  // GitHub user itsthisjustin.
  uint16_t estimatedTotalPages() const;
  // Load any page that has already been written during an active build, using the
  // in-memory LUT that grows with every onPageComplete(). Opens a temporary read handle
  // on the same file the build is writing to, syncing the writer first so the read handle
  // sees the latest committed pages (the writer is not synced per page). Must be called
  // between build slices, never concurrently with a slice on another task. Returns nullptr
  // on error. pageIndex must be < activeBuildPageCount().
  // `scratch`: see Page::deserialize -- the mid-build draw passes the build's lent region.
  std::unique_ptr<Page> loadPageFromActiveBuild(uint16_t pageIndex, BuildArena* scratch = nullptr);
  // Pre-decode every image in the section into its .pxc cache. Skips images that are
  // already cached or would show as a placeholder. The decode writes pixels into the
  // framebuffer as a side effect; call renderer.clearScreen() afterward. forceLoad
  // mirrors the effectiveForceLoad rule used at render time. alsoWarmGrayscale
  // additionally decodes the 4-level Bayer variant the AA grayscale planes replay;
  // see Page::warmImageCaches.
  //
  // NOT part of the normal open path: a section only needs image DIMENSIONS to lay out,
  // and those come from the ZIP entry header. Decoding a whole section up front cost 72 s
  // before the first page on an image-heavy book (X4), for pages the reader may never turn
  // to; the per-page warm in renderContents does the same work with the same policy, on
  // demand, borrowing the framebuffer as its arena. The one remaining caller is the
  // pre-reboot heap-recovery pass, which deliberately warms everything so the next boot can
  // render images with no decoder at all.
  // redecodeCoarse: see Page::warmImageCaches -- only for passes that run with the framebuffers
  // released, where the full progressive workspace fits.
  void warmAllImageCaches(int xOffset, int yOffset, bool forceLoad, bool monochromeOutput = true,
                          bool alsoWarmGrayscale = false, bool redecodeCoarse = false);
  bool isTruncatedCache() const { return truncatedCache; }
  bool isEmbeddedStyleFallback() const { return embeddedStyleFallback; }
  // True when the last build's CSS resolution hit low-heap skips (styles silently
  // missing from the cached pages), or when the loaded cache was written by such a build.
  bool isCssLowHeapDegraded() const { return cssLowHeapDegraded_; }
  // True when the last build demoted a table row to paragraphs, or the loaded cache was written
  // by such a build. The reader rebuilds such a chapter once per session when it is entered.
  bool isTableRowDegraded() const { return tableRowDegraded_; }
  // True when the build (or the loaded cache's build) exceeded a fixed-capacity limit and shows
  // less than the book: see ChapterHtmlSlimParser::CapOverflow.
  bool isSimplified() const { return simplified_; }
  // True when the last build's inline-footnote resolve pass failed, so some of this spine's
  // notes are missing from the store while the cache claims previews are on. Only meaningful
  // right after a build.
  bool isFootnotePreviewsUnresolved() const { return footnotePreviewsUnresolved_; }
  // True when the last build dropped an image to alt text on a heap refusal — transient, unlike
  // an unreadable image. Set right after a build, and also by loadSectionFile() from the cached
  // header, so a cold open can rebuild what a starved build left out.
  bool isImageHeaderDegraded() const { return imageHeaderDegraded_; }
  // True while an incremental build is in flight and its CSS resolver has ALREADY hit a
  // low-heap skip — i.e. the in-progress result is going to be css-degraded. Lets a sliced
  // caller (Background-B) abort early instead of finishing a build it will discard. False when
  // no build is live or no skips have occurred yet.
  bool activeBuildCssDegraded() const;

  // Given a page in this section, return the TOC index for that page.
  int getTocIndexForPage(int page) const;
  // Given a TOC index, return the start page in this section.
  // Returns nullopt if the TOC index doesn't map to a boundary in this spine (e.g. belongs to a different spine).
  std::optional<int> getPageForTocIndex(int tocIndex) const;

  struct TocPageRange {
    int startPage;  // inclusive
    int endPage;    // exclusive
  };
  // Returns the page range [start, end) within this spine that belongs to the given TOC index.
  std::optional<TocPageRange> getPageRangeForTocIndex(int tocIndex) const;

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  // Returns the printed-page label for a rendered page, wrapped in parens (e.g. "(42)"), if
  // one or more EPUB pagebreak markers / NCX <pageList> / page-map entries land on it.
  // Returns nullopt when no printed-page anchor falls on this exact page.
  std::optional<std::string> getPrintedPageLabelForPage(uint16_t page) const;
  // Like getPrintedPageLabelForPage but returns the most recent printed-page label at or
  // before `page` (raw label, no parens). Useful for pre-filling jump-to-page dialogs when
  // the current rendered page doesn't itself carry an anchor. Returns nullopt when no
  // printed-page anchor exists on this or any earlier page in the section.
  std::optional<std::string> getNearestPrintedPageLabelAtOrBefore(uint16_t page) const;

  // Standalone lookup that doesn't require a loaded Section. Walks the spine's cache bucket
  // (Epub::spineCacheDir), finds any cache variant for `spineIndex`, reads its printed-page label map,
  // and returns the parenthesised label for `page` if one is recorded. Returns nullopt when
  // no cache exists or the page carries no printed-page anchor. Used by SleepActivity to
  // augment the overlay without instantiating a full Section + render parameters.
  static std::optional<std::string> getPrintedPageLabelFromCache(const std::string& bookCachePath, int spineIndex,
                                                                 uint16_t page);

  // Look up the page number for a paragraph index (1-based, from XPath p[N]).
  // Uses the per-page paragraph LUT stored in the section cache.
  // Returns nullopt if the paragraph LUT is not available (old cache format) or pIndex is 0,
  // which is not a paragraph but the LUT's "no <p> seen yet" value.
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running <li> index (1-based, the Nth <li> at any depth
  // in the chapter). Used to snap KOReader-supplied list-item XPaths to a precise page
  // the same way getPageForParagraphIndex handles <p>-anchored XPaths.
  // Returns nullopt if the LUT is not available or the index is out of range.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the paragraph index for a given page number.
  // Returns the 1-based paragraph index of the last <p> element on or before the page.
  // Returns nullopt if the paragraph LUT is not available (old cache format) or the page has no
  // such <p> at all -- which is every page of a chapter whose paragraphs are not direct children
  // of <body> (`<body><div><p>`, what Calibre emits), since only body-child <p>s are counted.
  // Callers must treat that as "this page cannot be anchored on a paragraph" and fall back to
  // the page number; it is NOT an anchor on paragraph 0.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
  // The number of visible bytes of the chapter's source text before the page's first element
  // (VisibleText.h's rule, counted by the layout parser). What a KOReader sync pushes for the
  // page. nullopt if the LUT is unavailable or the page is beyond it (a truncated build).
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t page) const;
  // The end of the page's window of visible text: the first start of a LATER page that is strictly
  // greater than this page's start. Page starts can tie (an empty or image-only page is back-filled
  // with the next element's offset), so "the next page's start" would give an empty window.
  // nullopt if the page has no start or no later page starts beyond it (the last window, which
  // callers read as UINT32_MAX).
  std::optional<uint32_t> getVisibleTextOffsetAfterPage(uint16_t page) const;
  // The page a visible-text offset falls on: the last page whose start is <= offset, so a run of
  // pages sharing one start (an image or rule page before the text at that offset) answers its
  // last page, the text page; an offset below page 0's start (a chapter that opens with hidden
  // text) answers page 0. How a pulled KOReader position becomes a page. nullopt if the LUT is
  // unavailable or empty.
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t offset) const;

 private:
  // Allocates buildState_ and runs Setup. Applies the low-heap embedded-CSS downgrade.
  // requestedHash is the property hash of the params as requested by the caller (before
  // any CSS downgrade) — used to detect stale partial builds on later calls.
  // Returns false (with buildState_ cleared) on failure.
  bool startBuild(const BuildParams& params, const std::function<void(int)>& progressFn, uint32_t requestedHash);
};

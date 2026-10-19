#pragma once

// On-device render-benchmark harness (reader menu "Render benchmark" entry). Off by default —
// enable with -DENABLE_BENCHMARKS=1 in a platformio.ini environment for perf investigation
// builds only; the harness and its report-building code have no value for end users and are
// worth keeping out of release flash budgets.
#ifndef ENABLE_BENCHMARKS
#define ENABLE_BENCHMARKS 0
#endif

#include <BuildArena.h>
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <array>
#include <atomic>

#include "BookmarkStore.h"
#include "ChapterPageSpan.h"
#include "CrossPointState.h"
#include "EpubLinkBackStack.h"
#include "EpubProgressRecord.h"
#include "EpubReaderMenuActivity.h"
#include "ImageMemoryLadder.h"
#include "KOReaderAutoSync.h"
#include "KOReaderSyncWorker.h"
#include "ProgressMapper.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/TapTargets.h"

class EpubReaderActivity final : public Activity {
  // Reader can launch sync in several UX modes:
  // - COMPARE: legacy chooser (apply/upload) for power users.
  // - PULL_REMOTE / PUSH_LOCAL: direct one-step actions from menu entries.
  // - AUTO_PUSH: silent push on reader exit; skips when remote is already ahead.
  // Keeping this split in the caller avoids branching on menu semantics deep
  // inside generic reader state handling.
  enum class SyncLaunchMode {
    COMPARE,
    PULL_REMOTE,
    PUSH_LOCAL,
    AUTO_PUSH,
  };

  // Explicit spine/page/path override for launchKOReaderSync(), used when pushing progress from
  // the finished-book flow: by the time that flow runs, currentSpineIndex already points past the
  // last spine (see classifyRenderPass()'s FinishedBook check / the RowModel::Action::OpenNext
  // handler at end-of-book), so the live currentSpineIndex/section state no longer describes a
  // position to sync. Callers pass the last valid spine/page/pageCount they already computed
  // for the progress.bin write at finish. No paragraph/XPath hint is available for this
  // position, so mapping falls back to the same percentage-based estimate used for any position
  // without a LUT match — see docs/contributing/koreader-synchronization.md.
  // bookPath overrides epub->getPath(): the finished-book flow may have already moved the file to
  // /COMPLETED (SETTINGS.moveFinishedBooksToCompleted) before requesting the sync, and the live
  // `epub` object's path member does not follow a rename made outside of it. Empty means
  // "use epub->getPath() as before".
  struct SyncPositionOverride {
    int spineIndex;
    int page;
    int pageCount;
    std::string bookPath;
  };

  // Where launchKOReaderSync() should land once the sync (and the reboot it triggers to reclaim
  // WiFi-session heap fragmentation) completes — see KOReaderSyncPostAction in CrossPointState.h.
  // `target` is the book path (OpenBook) or OPDS author (OpdsSearch); unused otherwise. Deliberately
  // no default member initializer on `action` (KOReaderSyncPostAction::Reader == 0 covers it via
  // plain value-init) — one triggers a "required before the end of its enclosing class" error from
  // launchKOReaderSync()'s own `= SyncPostAction()` default argument, since that argument appears
  // textually before EpubReaderActivity (the enclosing class of this nested struct) is complete.
  struct SyncPostAction {
    KOReaderSyncPostAction action;
    std::string target;
  };

  // Encodes pending navigation intent — where to land once the target section is loaded.
  // Replaces the scattered nextPageNumber / pendingTocIndex / pendingAnchor /
  // cachedSpineIndex / cachedChapterTotalPageCount / pendingPercent* / pendingParagraph* fields.
  struct NavigationTarget {
    enum class Kind : uint8_t {
      Page,           // go to page n (0-based)
      LastPage,       // go to last page of section (was UINT16_MAX sentinel)
      Anchor,         // href fragment (e.g. "note1")
      TocIndex,       // TOC entry index
      Percent,        // normalised 0.0–1.0 within spine
      Paragraph,      // KOReader paragraph LUT index
      ListItem,       // KOReader li-anchored LUT index
      VisibleOffset,  // KOReader content offset (Section::getPageForVisibleTextOffset)
    };
    Kind kind = Kind::Page;
    union {
      int page;                // Kind::Page
      int tocIndex;            // Kind::TocIndex
      float spineProgress;     // Kind::Percent
      uint16_t lutIndex;       // Kind::Paragraph / Kind::ListItem
      uint32_t visibleOffset;  // Kind::VisibleOffset
    };
    std::string anchorStr;  // Kind::Anchor; empty for all others
    // Cross-font rescaling: page count of this spine at save time.
    // Non-zero for Kind::Page when loaded from progress.bin or written during reflow.
    // Also set for Kind::Paragraph / Kind::ListItem / Kind::VisibleOffset / Kind::Anchor so
    // a LUT miss still rescales the estimated fallbackPage instead of stranding at 0.
    int cachedPageCount = 0;
    int cachedSpineIdx = 0;
    // Estimated page used as a baseline before LUT/anchor lookup, and as a fallback
    // when the lookup misses. Only meaningful for Kind::Paragraph / Kind::ListItem /
    // Kind::VisibleOffset / Kind::Anchor — for Kind::Page the `page` field is the baseline.
    int fallbackPage = 0;
    // Kind::Page restored from progress.bin: the paragraph LUT index of the saved page (0 = none).
    // Used only once the chapter turns out to have been laid out anew (its page count differs from
    // cachedPageCount), in place of rescaling the page number. Otherwise the saved page stands: it
    // is exact, while the paragraph resolves to the page where that paragraph STARTS, a page early
    // whenever the reader had turned into a paragraph running over a page break.
    uint16_t restoreParagraph = 0;
    // Kind::Anchor from a followed link that pushed a Back entry: where it lands is checked once
    // against the Back stack (settleFollowedLink). Any other navigation assigns a new target, which
    // clears it.
    bool followedLink = false;

    NavigationTarget() : kind(Kind::Page), page(0) {}

    static NavigationTarget makePage(int n) {
      NavigationTarget t;
      t.kind = Kind::Page;
      t.page = n;
      return t;
    }
    static NavigationTarget makeLastPage() {
      NavigationTarget t;
      t.kind = Kind::LastPage;
      t.page = 0;
      return t;
    }
    static NavigationTarget makeAnchor(std::string a, int fallback = 0) {
      NavigationTarget t;
      t.kind = Kind::Anchor;
      t.page = 0;
      t.anchorStr = std::move(a);
      t.fallbackPage = fallback;
      return t;
    }
    static NavigationTarget makeTocIndex(int idx) {
      NavigationTarget t;
      t.kind = Kind::TocIndex;
      t.tocIndex = idx;
      return t;
    }
    static NavigationTarget makePercent(float sp) {
      NavigationTarget t;
      t.kind = Kind::Percent;
      t.spineProgress = sp;
      return t;
    }
    static NavigationTarget makeParagraph(uint16_t i, int fallback = 0) {
      NavigationTarget t;
      t.kind = Kind::Paragraph;
      t.lutIndex = i;
      t.fallbackPage = fallback;
      return t;
    }
    static NavigationTarget makeListItem(uint16_t i, int fallback = 0) {
      NavigationTarget t;
      t.kind = Kind::ListItem;
      t.lutIndex = i;
      t.fallbackPage = fallback;
      return t;
    }
    static NavigationTarget makeVisibleOffset(uint32_t offset, int fallback = 0) {
      NavigationTarget t;
      t.kind = Kind::VisibleOffset;
      t.visibleOffset = offset;
      t.fallbackPage = fallback;
      return t;
    }

    // Resolves the target into section.currentPage. Must be called on the render task
    // after the section has been loaded (pageCount is known).
    void resolveInto(Section& section, int spineIndex) const;
    // The page restoreParagraph lands on when `section` has been laid out anew since the position
    // was saved; nullopt when there is no paragraph, the layout is unchanged or the LUT misses.
    std::optional<int> relaidOutRestorePage(const Section& section, int spineIndex) const;
  };

  // Phase lifecycle for memory management at chapter boundaries.
  // READING:      normal state — section loaded, SD font metadata resident
  // PRECOMPILING: createSectionFile running — SD font metadata temporarily dropped
  enum class ReaderPhase : uint8_t { READING, PRECOMPILING };
  ReaderPhase readerPhase_ = ReaderPhase::READING;

  // One render() invocation services exactly one of these passes. The pass is
  // selected from the pending flags + reader state at entry by classifyRenderPass();
  // render() then dispatches to the matching helper. Replaces the former in-line
  // ladder of isPreRenderPass / isBufferDisplayPass / !section bool checks.
  enum class RenderPass : uint8_t {
    FinishedBook,     // currentSpineIndex == spineCount: hand off to FinishedBookActivity
    PreRender,        // Background A: render next page content into the framebuffer only
    BufferDisplay,    // fast page-turn: framebuffer already holds content; add status bar + flush
    BuildSection,     // no section loaded → build/load the cache, then render normally
    SectionBuilding,  // current section is being built incrementally (Background-C): draw the
                      // requested page from the in-progress LUT, or an "Indexing" popup if it
                      // isn't built yet. The build itself runs on the loop task, not here.
    Normal,           // section present → load page, render, display
  };

  // Resolved screen geometry for one render pass: oriented + padded margins and the
  // derived viewport. Computed once at the top of render() and threaded into the pass
  // helpers so each helper takes one struct instead of six loose ints.
  struct RenderLayout {
    int marginTop = 0;
    int marginRight = 0;
    int marginBottom = 0;
    int marginLeft = 0;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
  };

  std::shared_ptr<Epub> epub;
  // Build scratch backed by the borrowed secondary framebuffer (see the Background-C
  // release site). Handed to section->setExternalBuildScratch() so build allocations
  // land inside the lent block instead of the heap — nothing can allocate in the region,
  // so the return can never fail on a fragmented hole. Declared BEFORE `section`: the
  // section (whose in-flight build releases into this arena) must destroy first.
  std::unique_ptr<BuildArena> buildScratch_;
  // True while the secondary framebuffer is lent to buildScratch_ (borrowed, not freed).
  // Gates the returnSecondaryBuffer() path in recoverSecondaryBufferIfNeeded()/onExit.
  bool secondaryBorrowed_ = false;
  // True when the CURRENT holder of that borrow is Background-B rather than Background-C.
  // The two have opposite lifetimes: C's borrow lasts until its build ends (the reader is
  // waiting on it and has no AA to lose), while B's is pure look-ahead and must be handed
  // back the moment the reader needs the buffer again — AA is gated on a resident secondary
  // (see renderContents), so a borrow held across a page turn silently renders it BW.
  // Ownership transfers to C (this clears, secondaryBorrowed_ stays) when buildSection()
  // adopts B's in-flight section on a consecutive boundary cross.
  bool backgroundBorrowActive_ = false;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  // Where to land when this section is (re)built. INVARIANT: while a section is loaded this
  // names the page currently on screen, not the page the section was entered at — see
  // anchorNavTargetToCurrentPage().
  NavigationTarget navTarget;
  int pagesUntilFullRefresh =
      1;  // initialized to freq in onEnter(); 1 triggers HALF on first render if somehow not reset
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // When the reader last put a page on screen, for ANY reason — a turn, the first page of a
  // freshly opened book, a jump, a rebuild. Background-B's borrow quiet period measures from
  // here rather than from lastPageTurnTime, which is only stamped by actual page turns and so
  // reads as "settled forever" until the reader turns for the first time. Device-observed
  // (X3, 2026-08-11): B took the buffer 731 ms after a book's first page appeared and lost it
  // 513 ms later, because millis() - lastPageTurnTime was still measured from 0.
  unsigned long lastPageOnScreenMs_ = 0UL;
  bool pendingHalfRefreshAfterImagePage = false;
  // Force-refresh button: -1 = none, else a HalDisplay::RefreshMode to apply on the next
  // normal render of the CURRENT page (overrides the fast/half cadence for that one render).
  // Used by BTN_FORCE_REFRESH / BTN_FORCE_FAST_REFRESH so the user's manual refresh re-displays
  // the current page in the requested mode instead of raw-flushing a possibly pre-rendered buffer.
  int8_t forceRefreshModeNextRender_ = -1;
  // Refresh mode + display-mode byte of the page just shown, captured at its triggerDisplay
  // before the deferred-AA grayscale display overwrites the renderer's live last-mode. Logged
  // by the page summary so it reflects the page, not the AA pass.
  HalDisplay::RefreshMode lastPageRefreshMode_ = HalDisplay::RefreshMode::FAST_REFRESH;
  uint8_t lastPageDisplayModeByte_ = 0;
  // Page index/count of the page a display pass actually rendered, captured when that pass
  // begins. The page summary MUST log these rather than section->currentPage: pageTurn() runs
  // on the loop task without the render lock, so a turn arriving mid-render advances
  // currentPage before the summary is emitted and the render gets labelled with the page it is
  // about to be replaced by — which reads as the same page rendering twice.
  // The spine completes the identity for the Background-A re-arm, which may only retry the
  // pre-render of the page on screen (see stepBackgroundSectionBuild).
  int lastRenderedSpineIndex_ = -1;
  int lastRenderedPageIndex_ = 0;
  int lastRenderedPageCount_ = 0;
  // True when secondary display buffer allocation failed; while set we prefer
  // conservative refresh policy and skip grayscale AA to reduce ghosting.
  bool secondaryBufferDegraded_ = false;
  // Guard against repeated reboot attempts in a single reader session.
  bool fragmentationRecoveryRestartAttempted_ = false;
  // True for the duration of the per-page image warm pass (secondary buffer released,
  // JPEG/PNG decode in progress, secondary buffer realloc pending). RenderLock already
  // excludes Background-B/C from running during this window (they bail on
  // RenderLock::peek()); this flag is a second, explicit guard so that invariant doesn't
  // silently depend on renderContents() never yielding mid-warm-pass. Checked by
  // stepBackgroundSectionBuild() and stepCurrentSectionBuild() before they start any
  // heap-hungry work.
  bool imageProcessingActive_ = false;
  // The page drawn last with an image as a low-memory box (the memory ladder gave up on it, ImageMemoryLadder): its
  // redraws skip the decodes, the image lane leaves it, and the status bar shows the low-memory indicator while it is
  // on screen. -1: none.
  int memoryDegradedSpine_ = -1;
  int memoryDegradedPage_ = -1;
  // The page drawn last with a TrueType glyph that found no memory to be drawn in (glyph_miss): drawn once more by
  // itself, and the status bar shows the low-memory indicator while it is on screen with a gap. -1: none.
  int glyphStarvedSpine_ = -1;
  int glyphStarvedPage_ = -1;
  bool glyphStarvedRedraw_ = false;  // that redraw is owed by the pass that found the gap
  // The ladder left an accent family out (or found no font it could load): the next render lays the chapter out again
  // with the fonts loaded now, at the same position.
  bool fontRelayoutPending_ = false;
  // Arms forceHalfRefreshAfterPopup_ for the NEXT incremental section build, marking that build's
  // popup -> content transition as "dramatic" and worth a clean HALF baseline (vs. the routine,
  // frequent forward-reading crossing into a still-building Background-B section, where forcing
  // HALF made every section traversal pay a slow refresh). Two dramatic cases set it:
  //   - cold open of a book (set in onEnter()); valid only for the FIRST section entry, so any
  //     non-incremental entry — a cache hit or a blocking build — clears it (see buildSection),
  //     ensuring a cached re-open's first uncached forward crossing is NOT treated as dramatic.
  //   - a deliberate jump to a possibly-uncached section (chapter/percent/footnote): those sites
  //     arm a HALF refresh override via ReaderUtils::enforceExitFullRefresh(), which the indexing
  //     popup would otherwise consume, leaving the content page a FAST diff. buildSection detects
  //     that pending override (renderer.hasRefreshOverridePending()) and arms the post-popup HALF
  //     too, so the override paints the popup and the flag paints the content — both clean.
  bool coldOpenHalfRefreshArmed_ = true;
  // True after the "Indexing..." popup is drawn for a build flagged as a dramatic transition (see
  // coldOpenHalfRefreshArmed_), until the first real page replaces it on screen. That first page is
  // forced to HALF_REFRESH instead of the normal FAST cadence, establishing a clean baseline for the
  // popup -> content transition. Consumed by whichever path shows the first real page:
  // displayBuildPage() for a multi-slice build, or renderContents() directly when the build finishes
  // in a single slice (e.g. a one-page cover) and never goes through displayBuildPage at all.
  bool forceHalfRefreshAfterPopup_ = false;
  // When true, large images on the current page are decoded instead of shown as placeholders.
  // Reset to false on every page turn so the next image page starts with a placeholder again.
  bool forceLoadLargeImages = false;
  // Set after each render: true if the current page contains at least one placeholder image.
  bool pageHasPlaceholders = false;
  bool showTruncatedSectionHintThisRender = false;
  uint8_t truncatedSectionHintRendersRemaining = 0;
  int lastWarnedTruncatedSpineIndex = -1;
  // Spine whose image-degraded cache was already discarded for a rebuild this session (see the
  // cache probe in buildSection): one retry, not one per entry.
  int imageHeaderRebuildSpine_ = -1;
  // Same one-shot for a cache whose build demoted a table row or skipped CSS lookups on the
  // heap (Section::isTableRowDegraded / isCssLowHeapDegraded, persisted in the status byte).
  int degradedLayoutRebuildSpine_ = -1;
  struct RenderPhaseStats {
    unsigned long prewarmMs = 0UL;
    unsigned long bwRenderMs = 0UL;
    unsigned long displayMs = 0UL;
    unsigned long bwStoreMs = 0UL;      // unused (sequential path)
    unsigned long grayLsbMs = 0UL;      // LSB+MSB render+copy (planes phase)
    unsigned long grayMsbMs = 0UL;      // unused (sequential path)
    unsigned long grayDisplayMs = 0UL;  // displayGrayBuffer() waveform
    unsigned long bwRestoreMs = 0UL;    // BW re-render + cleanupGrayscaleWithFrameBuffer()
    unsigned long totalMs = 0UL;
  };
  struct LastRenderStats {
    bool valid = false;
    bool cacheRebuilt = false;
    bool usedGrayscale = false;
    bool hadImages = false;
    bool imagePageWithAA = false;
    bool forcedHalfRefresh = false;
    uint8_t orientation = 0;
    uint8_t imageRendering = 0;
    bool embeddedStyle = false;
    bool textAntiAliasing = false;
    int effectiveFontId = 0;
    int spineIndex = 0;
    int pageIndex = 0;
    int pageCount = 0;
    int footnoteCount = 0;
    int marginTop = 0;
    int marginRight = 0;
    int marginBottom = 0;
    int marginLeft = 0;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    unsigned long sectionLoadMs = 0UL;
    unsigned long pageLoadMs = 0UL;
    unsigned long requestRenderMs = 0UL;
    RenderPhaseStats phases;
    uint32_t freeHeapBefore = 0;
    uint32_t largestFreeBlockBefore = 0;
    uint32_t freeHeapAfter = 0;
    uint32_t largestFreeBlockAfter = 0;
    uint32_t fontCacheHits = 0;
    uint32_t fontCacheMisses = 0;
    uint32_t fontDecompressMs = 0;
    uint16_t fontUniqueGroups = 0;
    uint32_t fontPageBufferBytes = 0;
    uint32_t fontPageGlyphsBytes = 0;
    uint32_t fontPeakTempBytes = 0;
    uint32_t fontGetBitmapTimeUs = 0;
    uint32_t fontGetBitmapCalls = 0;
  };
#if ENABLE_BENCHMARKS
  struct BenchmarkAggregate {
    int renderCount = 0;
    int imagePageCount = 0;
    int cacheRebuildCount = 0;
    int maxFootnotes = 0;
    unsigned long totalRequestRenderMs = 0UL;
    unsigned long minRequestRenderMs = 0UL;
    unsigned long maxRequestRenderMs = 0UL;
    unsigned long totalRenderMs = 0UL;
    unsigned long minRenderMs = 0UL;
    unsigned long maxRenderMs = 0UL;
    unsigned long totalSectionLoadMs = 0UL;
    unsigned long totalPageLoadMs = 0UL;
    RenderPhaseStats totalPhases;
    uint32_t totalFontCacheHits = 0;
    uint32_t totalFontCacheMisses = 0;
    uint32_t totalFontDecompressMs = 0;
    uint32_t totalFontGetBitmapTimeUs = 0;
    uint32_t totalFontGetBitmapCalls = 0;
    uint32_t minFreeHeapAfter = 0;
    uint32_t maxFreeHeapAfter = 0;
  };
#endif  // ENABLE_BENCHMARKS
  LastRenderStats lastRenderStats;
  // Pre-rendered next page: frame buffer holds page content (no status bar) ready to display.
  // Set by the pre-render pass in render(); consumed and cleared by the fast path in pageTurn().
  // Invalidated (ready=false) on any navigation that is not a simple forward page turn.
  struct PreRenderedPage {
    bool ready = false;
    int spineIndex = -1;
    int pageIndex = -1;
    unsigned long renderDurationMs = 0UL;
    unsigned long completedAtMs = 0UL;
    // The draw ran short of memory and dropped glyphs. The framebuffer still holds this page
    // (so `ready` stays true for restoreCurrentPageToBufferIfPreRendered), but the page turn
    // must not show it: it renders the page afresh, by when the pass's own Page is gone.
    bool incomplete = false;
  };
  PreRenderedPage preRenderedPage;
  // The pre-render staged this page's grayscale planes, so displaying it can go
  // out as one waveform like a fresh render. Consumed (and cleared) by the
  // display; cleared with preRenderedPage.ready wherever the buffer is dropped,
  // because a stale plane pair belongs to a page that is no longer in the
  // framebuffer.
  bool preRenderedPlanesStaged_ = false;
  // Debug-only Background B (section pre-analysis) progress, surfaced as a small
  // status-bar overlay when DEBUG_BACKGROUND_WORK is enabled. Background A's state is
  // derived at draw time from pendingPreRender / preRenderedPage, so only B needs a
  // field. -1 means no background build is active.
  int8_t backgroundBuildPercent_ = -1;
  // --- Background B (idle build of the next consecutive sections' caches) ---
  // Lookahead cursor: spine index the B state below currently targets; -1 when B has no
  // target. Walks forward from currentSpineIndex+1 toward the book end as each target settles,
  // stopping once BG_BUILD_LOOKAHEAD_PAGES of runway is built ahead (see below), then idles
  // until navigation re-anchors the window.
  int backgroundBuildSpineIndex_ = -1;
  // Reading position the lookahead window is anchored at. When it no longer matches
  // currentSpineIndex (any navigation) the held B state is stale and the window restarts.
  int backgroundBuildBaseSpine_ = -1;
  // Pages B has laid out ahead in the current window (sum of pageCounts of the subsequent
  // sections built/cached since the last re-anchor). Combined with the current section's unread
  // tail, this is the runway the page-budget gate compares against BG_BUILD_LOOKAHEAD_PAGES.
  // Reset only on re-anchor; preserved across the per-target resetBackgroundBuild() in Settled.
  int backgroundWindowPagesBuilt_ = 0;
  // Section being built (or already built) for backgroundBuildSpineIndex_. Owned here
  // until buildSection() adopts it on a consecutive boundary cross or discards it on any
  // other navigation. Its destructor aborts a partial build and deletes the partial file.
  std::unique_ptr<Section> backgroundSection_;
  // True when the target spine's footnote links have never been scanned, so its build will run
  // the inline-preview resolve before laying out a line. Decided once in the Probe step; the pass
  // is sliced like the rest of the build, so all this buys is the BG_BUILD_RESOLVE_EXTRA_HEAP_BYTES
  // the resolver holds on the heap while it runs.
  bool backgroundBuildNeedsResolve_ = false;
  // Last WaitHeap gate evaluation; the heap-walk checks re-run at most ~1×/s.
  unsigned long backgroundBuildGateCheckMs_ = 0;
  // A clock-minute / battery status refresh held back while Background-B holds the secondary
  // buffer (shouldSkipPeriodicUpdate); endBackgroundBorrow requests it when the buffer returns.
  mutable bool statusRefreshDeferred_ = false;
  // Image lane (memory audit 2026-09, R7): between page turns, decode the pixel caches of the
  // images in a window of pages so the turn that reaches them replays a cache instead of running
  // a 1-4 s decode. Ranked above Background-B's look-ahead build. The window is the page on
  // screen (a large-image placeholder, or a decode the render could not afford, is loaded
  // lazily and the page redrawn) and the next kImageWarmLookahead pages, continuing into the
  // next section's first pages when its cache exists. The pair below marks a window already
  // found clean, so an idle reader does not re-read those pages every tick.
  static constexpr int kImageWarmLookahead = 5;
  int imageWarmCleanSpine_ = -1;
  int imageWarmCleanPage_ = -1;
  // Pages whose warm came back incomplete although no input preempted it (an unsupported or
  // corrupt image, a decode refused for heap). Each gets kImageWarmMaxTries attempts, then the
  // lane skips it for the session -- a failed decode writes no cache, so without this the lane
  // retried the same page on every loop tick. Eight slots hold every page of one window (the
  // page on screen plus five), so a window of broken images cannot evict its own entries.
  struct ImageWarmMiss {
    int16_t spine = -1;
    int16_t page = -1;
    uint8_t tries = 0;
  };
  static constexpr uint8_t kImageWarmMaxTries = 2;
  std::array<ImageWarmMiss, 8> imageWarmMisses_{};
  uint8_t imageWarmMissNext_ = 0;
  // When input last preempted a lane decode. Counts as activity for the settle rule: the press
  // that aborted the decode is usually still held (no new edge queued), so without this the lane
  // restarted on the very next tick and was aborted again by the release, delaying the page turn
  // the press was for (~260 ms on the X3, 2026-09-26).
  unsigned long imageWarmPreemptedMs_ = 0;
  // Times a build of backgroundBuildSpineIndex_ was preempted (reader needed the borrowed
  // buffer back) before reaching Done. Bounds the retry loop: a spine whose parse cannot fit
  // between two page turns would otherwise re-inflate and re-parse forever, burning CPU, SD
  // writes and battery for a cache it never finishes. After BG_BUILD_MAX_PREEMPTIONS the
  // target is abandoned to Background-C and the cursor moves on. Reset per target.
  uint8_t backgroundPreemptCount_ = 0;
  // The last background build ran short of memory -- degraded, truncated or failed -- and was
  // skipped (the foreground builds that chapter when the reader gets there). B then waits for the
  // reader to enter the next chapter before it tries another, so a book that never fits costs
  // one wasted attempt per chapter entered, not one per target: it used to switch B off for the
  // book after three in a row (a 154-page PDF conversion had run 132 builds, 8 kept, each ~600 ms
  // and a 55-62 KB partial file written and deleted). Cleared when the reading position moves.
  bool backgroundPausedForChapter_ = false;
  // The free heap when B took the buffer, and whether the allocator is tracking the low point
  // since (heap_caps_monitor_local_minimum_free_size_start): logged when the buffer goes back.
  uint32_t backgroundBorrowFreeAtStart_ = 0;
  bool backgroundHeapLowTracked_ = false;
  // B is building the next chapter while the reader is on the last few pages of this one, so its
  // borrow survives page turns instead of being taken back by every render (see
  // BG_BUILD_THROUGH_PAGES). Those pages draw in BW with no pre-render. Cleared when the buffer
  // goes back or the build passes to the foreground.
  bool backgroundBuildThrough_ = false;
  // The next chapter, abandoned to BG_BUILD_MAX_PREEMPTIONS while the reader was mid-chapter. It
  // gets one more try once the reader is on its last pages, in build-through mode, instead of
  // being left to the foreground "Indexing". -1 when none; cleared when the reader moves on.
  int backgroundAbandonedSpine_ = -1;
  // One-shot Background-A re-arm latch (see serviceBackgroundWork): the (spine, page)
  // whose pre-render was already retried after the deferred AA released its memory.
  // Bounds retries to one per displayed page so an image-only next page (which can
  // never produce a pre-render) doesn't re-trigger the pass every idle tick.
  int preRenderRearmSpine_ = -1;
  int preRenderRearmPage_ = -1;
  // Escalation latch for a page that will not load off the SD (see renderNormalPass). A null
  // Page is overwhelmingly a transient allocation failure in Page::deserialize (~10.5 KB of
  // small allocations, the one reader path with no arena), not a damaged cache — so the first
  // failure must not delete the chapter's cache file and charge the reader a full re-index.
  // Keyed on (spine, page) so any successful load or navigation clears it implicitly.
  //   0 — no failure recorded here
  //   1 — reloaded the section without touching the cache (transient hypothesis)
  //   2 — cache cleared and rebuilt (structural hypothesis); a third failure gives up
  //       rather than looping, which is the TODO that used to sit at that call site.
  int pageLoadFailSpine_ = -1;
  int pageLoadFailPage_ = -1;
  uint8_t pageLoadFailStage_ = 0;
  // One-shot-per-target probe/settle latch so idle ticks don't re-hit the SD every loop:
  //   Probe    — not yet checked whether the target's cache already exists
  //   WaitHeap — cache missing; waiting for the heap gates to pass (rechecked each tick)
  //   Building — backgroundSection_ has an in-flight incremental build
  //   Settled  — done for this target (built / cached / failed); advances the cursor to the
  //              next section in the lookahead window (or idles if the window is exhausted)
  enum class BackgroundBuildState : uint8_t { Probe, WaitHeap, Building, Settled };
  BackgroundBuildState backgroundBuildState_ = BackgroundBuildState::Probe;
  // WaitHeap's last gate check found the reader settled and no input queued, so only the heap or
  // the buffer refused: B may wait like this for the rest of the chapter. Read by
  // hasPendingBackgroundWork(), which then stops calling B pending.
  bool backgroundBuildStarved_ = false;
  // Set between suspendBackgroundWork() and resumeBackgroundWork(): no look-ahead
  // build is probed, armed or stepped, and no page is pre-rendered.
  bool backgroundWorkSuspended_ = false;
  // The build state behind skipLoopDelay() / preventAutoSleep(), as last read under the render
  // lock. Loop task only: main.cpp polls both hooks from there, and refreshLoopHints() is their
  // only writer.
  bool loopHintCurrentBuild_ = false;     // section && section->hasActiveBuild()
  bool loopHintBackgroundBuild_ = false;  // backgroundBuildState_ == Building
  void refreshLoopHints();
  // --- Background C (incremental build of the CURRENT section while the reader watches) ---
  // When the spine the user just entered has no cache, buildSection() starts an in-place
  // incremental build owned by `section` and hands the slicing to stepCurrentSectionBuild()
  // on the loop task, so input stays responsive. The render task only ever draws (the
  // SectionBuilding pass). State below coordinates that hand-off.
  //
  // Page last drawn by the SectionBuilding pass (-1 = none yet / popup showing). Stops the
  // pass redrawing the same page every idle tick.
  int buildDisplayedPage_ = -1;
  // True once the "Indexing" popup has been drawn for the current pending page, so the pass
  // doesn't re-flush it every tick. Cleared whenever a real page is drawn or the build ends.
  bool buildingPopupShown_ = false;
  // Spine for which the incremental build failed/degraded and must be retried with the old
  // blocking (secondary-buffer-released) path instead of Background-C. -1 = no such latch.
  int forceBlockingBuildSpine_ = -1;
  // Spine whose RESIDENT Background-C build aborted on the proactive low-heap guard and must be
  // retried as IncrementalReleased (buffer freed, still sliced — the first page still appears
  // mid-build). Distinct from forceBlockingBuildSpine_: a low-heap abort is a headroom problem
  // the release solves, not a parse failure, so it must not collapse to the blocking path
  // (which indexes the whole section before showing anything). -1 = no such latch.
  int forceReleasedBuildSpine_ = -1;
  // Debug-only Background A glyph for the status-bar overlay. The transient flags
  // (pendingPreRender / preRenderedPage.ready) are cleared at the top of render()
  // before the status bar is drawn, so the overlay could never sample a non-idle
  // state. Instead we latch per-page provenance here just before the status bar
  // draws: 'x' = this page was served from the pre-render buffer (a Background-A
  // hit), '-' = it was rendered fresh (a miss). Drawn only under the flag.
  char backgroundAGlyph_ = '-';
  // Debug-only counters flushed to serial every ~5s by serviceBackgroundDebugLog() when
  // DEBUG_BACKGROUND_WORK is enabled. "Runs" counts how often each background routine was
  // invoked with work to do; "completes" counts how often it finished a unit (a pre-rendered
  // page for A, a fully built section for B). Cheap monotonic counters; no release-build cost
  // since the logger is compiled out when the flag is 0.
  struct BackgroundWorkCounters {
    uint32_t aRuns = 0;       // pre-render pass entered with a page to render
    uint32_t aCompletes = 0;  // pre-render produced a ready next page
    uint32_t bRuns = 0;       // Background-B (look-ahead) ran a build slice
    uint32_t bCompletes = 0;  // Background-B build reached Done
    // Background-C (the section the reader is waiting on) is counted SEPARATELY. It used to
    // share B's pair, which made the debug line unreadable exactly when it mattered: a long C
    // build over a big spine printed "B runs=211" while B's own state said probe/spine=-1, so
    // the one counter you would check to see whether look-ahead is working was being driven by
    // something else entirely.
    uint32_t cRuns = 0;
    uint32_t cCompletes = 0;
  };
  BackgroundWorkCounters bgCounters_;
  unsigned long lastBgDebugLogMs_ = 0UL;
  // Emit the background-work counters to serial roughly every 5s. No-op unless
  // DEBUG_BACKGROUND_WORK is set. Called from loop().
  void serviceBackgroundDebugLog();
  struct PageTurnStatsWindow {
    uint16_t turns = 0;
    uint16_t preRenderHits = 0;
    uint16_t preRenderMisses = 0;
    unsigned long totalPreRenderMs = 0UL;
    unsigned long totalIdleSlackMs = 0UL;
  };
  static constexpr uint16_t PAGE_TURN_STATS_WINDOW_SIZE = 10;
  PageTurnStatsWindow pageTurnStatsWindow;
  // Set by render() after a normal page render to request a pre-render of the next page.
  bool pendingPreRender = false;
  // Deferred grayscale (Phase 8): after the BW display pass, defer the AA render until the
  // next idle loop tick (no navigation input pending). This makes rapid page turns feel faster
  // — only the BW pass runs while flipping; AA runs once when the user pauses.
  // Cleared by loop() after running the deferred pass, and on every page turn.
  struct PendingGrayscale {
    bool active = false;
    std::shared_ptr<Page> page;  // page whose text needs the AA pass
    int fontId = 0;
    int marginLeft = 0;
    int contentTop = 0;
    bool fastLut = false;
  };
  PendingGrayscale pendingGrayscale_;
  // The reading position the owed deferred AA and the armed pre-render belong to: the page on
  // screen when either was armed (every arming site runs with that page current). render() and
  // the deferred-AA runner drop both once the position has moved, so a navigation that does not
  // make pageTurn()'s hand-off (10-page jump, chapter jump, any future one) still gets its page
  // drawn instead of having its render shelved as a pre-render of the page it left.
  int stagedForSpine_ = -1;
  int stagedForPage_ = -1;
  void markStagedForCurrentPage() {
    stagedForSpine_ = currentSpineIndex;
    stagedForPage_ = section ? section->currentPage : -1;
  }
  // True (and the staged work dropped) when the position has moved since it was staged.
  bool dropStagedWorkIfPositionMoved(const char* where);
  // Set by pageTurn() fast path to tell render() the frame buffer already holds the next page
  // content and only the status bar + display flush are needed.
  bool usePreRenderedBuffer = false;
  // Progress save is posted by render() and consumed by loop() to keep SD I/O off the render task.
  // render() writes spineIndex/page/pageCount then sets pending with release semantics so loop()
  // sees a coherent snapshot when it observes pending==true via acquire.
  struct PendingProgressSave {
    std::atomic<bool> pending{false};
    int spineIndex = 0;
    int page = 0;
    int pageCount = 0;
  };
  PendingProgressSave pendingProgressSave;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool finishedBookActivityStarted_ = false;
  // Armed by renderFinishedBookPass() (render task), consumed by loop() (loop task). The launch
  // itself must not run on the render task: it ends in pushActivity(), which writes
  // ActivityManager's pendingActivity/pendingAction — and loop() reads and std::move()s those
  // with no lock at all, because the invariant they rely on is "only the loop task touches
  // them", not "the render lock covers them". A unique_ptr assigned from one task while another
  // moves it is a double-free waiting to happen.
  std::atomic<bool> finishedBookLaunchPending_{false};
  // Last valid spine/page/pageCount at book-finish time, stashed for onFinishedBookSyncRequested()
  // — see the SyncPositionOverride comment for why currentSpineIndex/section can't be used there.
  int finishedBookSyncSpineIndex_ = 0;
  int finishedBookSyncPage_ = 0;
  int finishedBookSyncPageCount_ = 0;
  ReaderUtils::InputDrainGuard inputDrainGuard;
  bool automaticPageTurnActive = false;
  // Pages turned in the current reader session. Used to gate auto-push-on-close: a brief
  // inspection of a book should not trigger a network round-trip. Reset on every reader
  // entry; not persisted, since "session" means the lifetime of this activity instance.
  int sessionPagesAdvanced = 0;

#if CROSSPOINT_KOREADER_AUTOSYNC
  bool autoSyncPullPending = false;
  unsigned long autoSyncLastPushAt = 0;
  uint64_t autoSyncPushSeq = 0;
  uint64_t autoSyncPullSeq = 0;
  KOReaderSyncJob autoSyncPullJob;
  bool autoSyncPullJobPending = false;
  bool autoSyncPullDialogLaunched = false;
  SyncIndicator autoSyncIndicator = SyncIndicator::None;
  void refreshAutoSyncIndicator();

  void serviceAutoSync();
  bool autoSyncReaderIsQuiet() const;
  void maybeAutoPullOnWake();
  bool pollAutoSyncPull();
  bool evaluateAutoSyncPull();
  bool handOffToInteractiveSync();
  void silentUploadCurrentPosition();
  void silentApplyRemote(const KOReaderProgress& remote);
  void maybeAutoPushInterval();
  void pollAutoSyncJob();
  void maybeAutoPushOnSleep();
  void releaseAutoSyncSlot();
  KOReaderPosition currentKoPosition(int page, int pageCount) const;
#endif  // CROSSPOINT_KOREADER_AUTOSYNC

  // -1 means use global SETTINGS value.
  int8_t bookEmbeddedStyleOverride = -1;
  int8_t bookImageRenderingOverride = -1;
  int8_t bookFontFamilyOverride = -1;
  std::string bookSdFontFamilyOverride;
  int8_t bookFontSizeOverride = -1;
  int8_t bookBionicReadingOverride = -1;
  int8_t bookParagraphAlignmentOverride = -1;
  int8_t bookTextAntiAliasingOverride = -1;
  int8_t bookHyphenationOverride = -1;
  int8_t bookFontSizeNormalizationOverride = -1;
  int8_t bookGuideDotsOverride = -1;
  int8_t bookInlineFootnotePreviewsOverride = -1;

  // Bookmarks (starred pages)
  BookmarkStore bookmarkStore;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  // Where a footnote jump came FROM, so page-back returns to the caller instead of to the
  // previous page (objective: a note is a detour, not a place in the reading order). Anchored on
  // the caller's paragraph rather than its page number: any repagination between the jump and the
  // return — a font change made from inside the note, a background rebuild — moves page numbers,
  // and landing a page or two off is exactly the disorientation the return is meant to prevent.
  // pageNumber/pageCount stay as the proportional fallback for a spine with no paragraph LUT --
  // and for the far more common case of a book whose <p>s are not direct children of <body>
  // (`<body><div><p>`, what Calibre emits), where no page has an anchorable paragraph at all and
  // getParagraphIndexForPage answers nullopt for every one of them.
  using SavedPosition = EpubLinkBackStack::Entry;
  static constexpr int MAX_FOOTNOTE_DEPTH = EpubLinkBackStack::kMaxDepth;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;
  // How many of those entries were pushed in this reader session (EpubLinkBackStack::push). A stack
  // loaded from linkstack.bin leaves it at 0: Back uses footnoteDepth, but the KOSync push on sleep
  // is skipped only for a link followed this session, as it was before the stack was persisted.
  int sessionLinkDepth_ = 0;
  // True while linkstack.bin exists and holds exactly the stack above. The first push or pop after
  // that deletes the file, and the reader's exit writes the stack only while this is false -- so the
  // file is written when the stack changed, never per page turn, and never disagrees with memory.
  // An unclean shutdown (crash, flat battery) can therefore lose a stack but never bring back one
  // the reader had already returned from, which would be worse than none: Back would jump from
  // wherever they had read on to an origin they had already left behind.
  bool linkStackOnDisk_ = false;

  // --- render() pass dispatch (see RenderPass) ---
  // Opportunistically restore the secondary display buffer if a prior OOM degraded it.
  void recoverSecondaryBufferIfNeeded();
  // The reader is within BG_BUILD_THROUGH_PAGES of the end of the chapter on screen, on a panel
  // that can show pages while Background-B holds the secondary buffer: one whose FAST refresh
  // needs no host-side previous frame (GfxRenderer::fastRefreshNeedsHostFrame() false, the X3).
  bool nearChapterEndForBuildThrough() const;
  // Realloc the secondary buffer, evicting rebuildable caches (FDC page slots, CSS
  // resolve caches) that a released build may have planted inside the freed hole and
  // retrying once before reporting failure. Shared by the post-build and opportunistic
  // recovery paths.
  bool reallocSecondaryEvictingCaches();
  // Walk the image headers the manifest still has queued (walks that were short of memory) from
  // the borrowed secondary framebuffer, the one region of full-ring size a reader session can
  // always get. No-op without a resident buffer, a queue, or while a background build holds the
  // borrow. Returns true when at least one image was resolved.
  bool resolvePendingImageHeadersFromFramebuffer();
  // Note text for each of currentPageFootnotes, for the footnote list activity (empty strings
  // where the store has no entry). A pure read: note text is resolved by the section build that
  // needs it, never by opening the list.
  // Note text and kind for each of currentPageFootnotes, for the footnote list. One pass over one
  // open store: a hit gives the preview AND says the link is a note, a miss on a scanned spine
  // says it is navigation.
  struct PageLinkInfo {
    std::vector<std::string> previews;  // empty where the store has no text
    std::vector<uint8_t> isNote;        // 1 = footnote, 0 = navigation
  };
  PageLinkInfo pageLinkInfoForCurrentPage();
  // Clamp currentSpineIndex into [0, spineCount]. spineCount itself is the finished-book sentinel.
  void clampSpineIndex(int spineCount);
  // Compute oriented + padded margins and the derived viewport for this render.
  RenderLayout computeRenderLayout() const;
  // Select which pass this render() invocation should run, from pending flags + reader state.
  RenderPass classifyRenderPass() const;
  // FinishedBook pass: arm the transition to the finished-book flow. Runs under the render
  // lock and keeps it; the launch itself is deferred to the loop task (see the pair below).
  void renderFinishedBookPass(int spineCount);
  // Loop-task half of renderFinishedBookPass(): performs the launch it armed.
  void serviceFinishedBookLaunch();
  // PreRender pass (Background A): render the next page's content into the framebuffer only.
  void renderPreRenderPass(const RenderLayout& layout);
  // BufferDisplay pass: framebuffer already holds the next page; add status bar + flush.
  // Returns true if the page was displayed (render() should return); false if the fast path
  // could not be taken (load failed / image page) and render() must fall through to Normal.
  bool renderBufferDisplayPass(const RenderLayout& layout);
  // BuildSection pass: construct/load the section cache for currentSpineIndex and resolve the
  // nav target. Returns true if a section is ready to render; false if render() should return
  // (build failed, finished-book handoff, or a requestUpdate retry was posted).
  bool buildSection(const RenderLayout& layout);
  // Outcome of compileSectionCache(). Restarting means a fragmented-heap recovery reboot was
  // triggered and the caller must return immediately without further work.
  enum class BuildOutcome : uint8_t { Built, Failed, Restarting };
  // Draws the indexing popup, then builds the section cache file for the current section.
  // Prefers an in-place build that keeps the secondary display buffer (preserving the fast-
  // refresh baseline); falls back to releasing the buffer — for parse/decode headroom — when
  // heap is tight or the in-place attempt fails. On the released path it pre-decodes images
  // eagerly and reallocates the buffer (arming a half-refresh); the in-place path defers images
  // to lazy per-page decode and leaves the baseline intact for a normal fast refresh.
  BuildOutcome compileSectionCache(const RenderLayout& layout, bool embeddedStyle, uint8_t imageRendering);
  // True when heap is ample enough to build the current section WITHOUT releasing the secondary
  // buffer (the in-place path). Reuses Section::heapAllowsEmbeddedStyle for CSS books.
  // inflatedSize is the spine's uncompressed size: the extraction phase holds an inflate ring
  // sized to the entry (≤32 KB), a per-spine cost the static floors don't cover — a big spine
  // must clear floor+ring or the resident build is doomed to the low-heap abort. 0 = unknown
  // (fall back to the static floors alone).
  bool heapAllowsInPlaceBuild(bool embeddedStyle, size_t inflatedSize) const;
  // Normal pass: load the current page from the section cache, render it, persist progress.
  void renderNormalPass(RenderLock& lock, const RenderLayout& layout);
  // SectionBuilding pass: while Background-C builds the current section, draw the requested
  // page from the in-progress LUT once it's been written (text-only pages only), or an
  // "Indexing" popup until then. Does no build work itself — that's stepCurrentSectionBuild().
  void renderSectionBuildingPass(RenderLock& lock, const RenderLayout& layout);

  // --- idle-time background work ---
  // Called by loop() when input is idle and no page turn is pending. Dispatches the
  // cooperative background routines in priority order, giving each a small time slice:
  //   1. deferred grayscale AA (visible quality of the page just shown)
  //   2. Background A — pre-render of the next page (scheduled via pendingPreRender)
  //   3. Background B — idle section pre-analysis (only once A is finished; not yet implemented)
  // Each routine must be partial-work-capable and yield promptly so the next loop tick
  // can service input. Returns nothing; routines self-gate on their own pending flags.
  void serviceBackgroundWork();
  // Runs the deferred grayscale AA pass for the page just displayed, if one is pending and
  // the display bus is free. Serialises against the render task via RenderLock. No-op when
  // nothing is pending. Extracted from loop()'s idle branch.
  void runDeferredGrayscalePass();
  // Frees a large SD font page arena (PAGE_ARENA_RELEASE_MIN_BYTES) once its page is on the panel:
  // after the AA pass, a pre-render, or a render that owes no AA. Called under the render lock.
  void releasePageGlyphArena(const char* after);
  // The pre-render's heap floor: PreRenderGate's from the costs measured so far, raised by the arena the last release
  // freed, which the pre-render would allocate again on top.
  uint32_t preRenderMinFreeHeap(bool inkOnly) const;
  // The pre-render can keep 1-bit ink masks (SdCardFont::setInkOnlyPrewarm) instead of 2-bit glyphs:
  // its draw is black and white only. Not on a single-push panel (its pre-render captures the grey
  // planes from the same glyphs), and only for a native-size SD font, the font that has an arena.
  bool inkOnlyPrerenderAvailable() const;
  // Every glyph on the page is drawn at the font's own size: a scaled draw (a heading multiplier,
  // a sized span, sup/sub, small caps) resamples the 2-bit levels, which an ink mask has lost.
  static bool pageDrawsUnscaled(const Page& page);
  uint32_t releasedPageArenaBytes_ = 0;
  // The most a pre-render pass has cost in this book, measured by the pass (PreRenderGate), and apart the most one cost
  // that drew glyphs the glyph store did not hold yet; 0 until one is. And whether the page drawn last drew such.
  uint32_t preRenderCostMax_ = 0;
  uint32_t preRenderMissCostMax_ = 0;
  bool lastPageDrewMisses_ = false;
  // Background B: advance the idle build of the next consecutive section by one bounded
  // step (state probe, heap gate, or one ~BG_BUILD_BUDGET_MS parse slice). Runs only after
  // Background A has had its slice (A keeps priority: it drives perceived page-turn speed).
  // Serialises SD access against the render task via RenderLock; skips the tick instead of
  // blocking when the render task is busy.
  void stepBackgroundSectionBuild();
  // The image lane's step; called from stepBackgroundSectionBuild with the RenderLock held.
  // True when it did a page's worth of work this tick (the caller then yields to the loop).
  bool stepImageWarmLocked();
  // Decode one page's missing image caches into the borrowed secondary buffer, writing no
  // framebuffer byte. `onScreen`: the page is the one displayed; once the images it showed as
  // placeholders are cached it is redrawn. Always returns true (the tick was spent).
  bool warmPageForImageLane(const Page& page, int spine, int pageIndex, bool onScreen, bool warmGrayscale);
  // The memory ladder (ImageMemoryLadder): warms `page`'s missing image caches at (xOffset, yOffset), and while a
  // decode is short of memory, decodes again with font memory freed, step by step. `arena` is the decode scratch
  // (the lent framebuffer), rewound between steps. `drawsPage`: the page is drawn next, so its glyphs are put into
  // the glyph store before the light step frees what a miss needs. `accents`: the book's accent families loaded
  // beside the body. Returns where it ended: Done, Emergency, or what finishLadder() is for.
  ImageMemoryLadder::Action warmImagesClimbingLadder(const Page& page, int xOffset, int yOffset, bool forceLoad,
                                                     bool warmGrayscale, BuildArena* arena, bool drawsPage,
                                                     uint8_t accents);
  // The ladder's DropAccent and Restart, once the decode scratch is home: the book keeps one accent family fewer,
  // its fonts are loaded again and the next render lays the chapter out; or the device restarts to the reader. True
  // when the page must not be drawn now.
  bool finishLadder(ImageMemoryLadder::Action action, uint8_t accents);
  // The scan pass over `page`'s text and the prewarm of every font and style it uses: TrueType glyphs into the glyph
  // store, built-in ones into the decompressor's page slots.
  void prewarmPageFonts(const Page& page, int xOffset, int yOffset);
  bool memoryDegradedPage(int spine, int page) const {
    return memoryDegradedSpine_ == spine && memoryDegradedPage_ == page;
  }
  bool glyphStarvedPage(int spine, int page) const { return glyphStarvedSpine_ == spine && glyphStarvedPage_ == page; }
  bool imageWarmGaveUp(int spine, int page) const;
  void noteImageWarmMiss(int spine, int page);
  // Lend the secondary framebuffer to Background-B's build arena. Mirrors the Background-C
  // borrow site in buildSection(): the lent block never enters the heap, so the return cannot
  // fail on a fragmented hole, and the build's scratch — parse working set, inflate ring, CSS
  // index — bump-allocates inside it instead of competing for the ~24 KB the reading heap has
  // left. Sets the single-buffer display flags AA/fast-diff need while the buffer is away.
  // Returns false (changing nothing) when there is no secondary buffer to lend or the arena
  // could not be constructed. Requires backgroundSection_ to exist and its build not started.
  bool beginBackgroundBorrow();
  // Hand the borrowed buffer back to the display and restore the normal AA/fast-diff paths.
  // Discards Background-B's in-flight build first — not optional: the build's live state
  // (parser, inflate ring, CSS index) points into the lent region, which is about to become
  // a framebuffer again. No-op unless Background-B currently holds the borrow. The one path
  // that must NOT call this is the buildSection() adoption, where the borrow transfers to
  // Background-C with the build intact; it clears backgroundBorrowActive_ directly instead.
  void endBackgroundBorrow();
  // Background C: advance the incremental build of the CURRENT section (the one the reader is
  // looking at) by one bounded slice. Runs only while `section` has an active build, with the
  // highest reader-build priority (ahead of A and B). On a newly built target page or on
  // completion it posts a requestUpdate() so the render task draws it; on completion it resolves
  // the nav target and transitions back to READING; on a failed/degraded build it latches a
  // blocking-rebuild fallback. Serialises against the render task via RenderLock.
  void stepCurrentSectionBuild();
  // How the current section's (re)build should run. Resident/Released are both incremental
  // Background-C builds (responsive, build-while-you-read); they differ only in whether the
  // secondary buffer stays in RAM or is freed for headroom on a tight heap. Blocking is the old
  // synchronous path, used only when C can't apply (latch set, no buffer, or a CSS-fallback
  // rebuild — decided by the caller).
  enum class SectionBuildMode : uint8_t {
    Blocking,             // synchronous build (no mid-build display)
    IncrementalResident,  // Background-C, secondary buffer kept resident
    IncrementalReleased,  // Background-C, secondary buffer released for headroom, restored after
  };
  // Pick the build mode (blocking-fallback latch / buffer presence aside). X3 always releases
  // (its baseline lives in the controller, so a resident buffer only starves the build); X4
  // releases for CSS books (resident reliably css-degrades) and for non-CSS books that don't fit
  // the in-place floors, keeping the buffer resident only for non-CSS builds that do fit.
  // inflatedSize: the spine's uncompressed size (see heapAllowsInPlaceBuild); 0 = unknown.
  SectionBuildMode chooseSectionBuildMode(bool embeddedStyle, size_t inflatedSize) const;
  // Render params for a section build of `spineIndex`, identical to what buildSection()
  // passes to createSectionFile — B must build the exact variant the foreground will load.
  Section::BuildParams makeSectionBuildParams() const;
  // The book's other font families, which makeSectionBuildParams() fills and points the params at: a build keeps
  // its params across slices, so the table lives as long as the reader.
  mutable BlockFontFamilies bookFontFamilies_;
  // Drop all Background-B state (aborting a partial build and deleting its partial cache
  // file via ~Section). Resets the overlay percent.
  void resetBackgroundBuild();

  void renderContents(RenderLock& lock, std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  // Renders page content into the frame buffer (prewarm + BW pass) without drawing the status bar
  // or flushing to the display. Used by the pre-render pass so the status bar can be superimposed
  // at display time with live values (clock, battery). Returns the number of glyphs the font
  // decompressor could not find memory for -- they drew nothing, so the page is incomplete.
  uint16_t renderPageContentOnly(const Page& page, int orientedMarginTop, int orientedMarginRight,
                                 int orientedMarginBottom, int orientedMarginLeft);
  // Draws a single text-only page from an in-progress Background-C build (no AA, no pre-render
  // arming). Releases the lock before the waveform wait (like renderContents) so a C build
  // slice can run on the loop task during the refresh.
  // `drawBlock`: the arena block the page's TextBlock bytes live in, when the caller loaded the
  // page from the build's lent region. Released here, before the lock is, so a build slice that
  // runs during the waveform wait finds the arena cursor where it left it.
  void displayBuildPage(RenderLock& lock, const Page& page, const RenderLayout& layout,
                        BuildArena::Block* drawBlock = nullptr);
  // Draws the status bar over the current frame buffer and flushes to the display.
  // Handles the refresh cycle and grayscale AA pass. page must be the same page
  // that was last rendered into the buffer (needed for image AA re-render).
  void displayPreRenderedPage(const Page& page, int orientedMarginTop, int orientedMarginRight,
                              int orientedMarginBottom, int orientedMarginLeft);
  // True when the page currently being rendered is already on its way out, so its
  // anti-aliasing touch-up (cosmetic, and uncancellable once started) should be dropped.
  // Covers the whole gesture, not just its tail: the sampler seeing a press edge, a
  // navigation button being held, and an update already queued by a completed turn.
  // Advisory — call it from the render task as late as possible, and never gate anything
  // the next frame depends on it.
  bool aaPreemptedByNavigation() const;

  // True when this panel runs the AA pass deferred (from the idle loop) rather
  // than inline (hidden inside a running waveform). Inline needs a panel that can
  // genuinely overlap -- see GfxRenderer::supportsAsyncRefresh -- and the X3
  // defers even though it can, for the scheduling reasons the pre-render guards
  // below describe.
  //
  // Every deferred-AA user needs those guards: the pass is cancelled by a queued
  // update, so a pre-render armed before it runs preempts it *every time* and the
  // AA is never applied. That is not an X3 property, it is a property of
  // deferring, which is why this is one predicate and not a board name.
  bool usesDeferredAa() const;
  // If the frame buffer currently holds a pre-rendered next page, redraw the current page
  // into it (no status bar, no flush). Restores the invariant other activities — notably
  // SleepActivity's OVERLAY mode — rely on when transitioning out of the reader.
  void restoreCurrentPageToBufferIfPreRendered();
  void renderStatusBar() const;
  // The book percentage and the chapter title the status bar shows; section must be loaded.
  float statusBarBookPercent() const;
  std::string chapterTitle() const;
  // The status bar's chapter page counter: the current file's page and total, widened to the
  // whole TOC chapter when that spans several spine items (#325; see ChapterPageSpan).
  ChapterPageSpan::Display chapterPageDisplay() const;
  // The status bar's printed-page label for `page`: the label landing on it, else the last one
  // passed in this section, both parenthesised. Empty when the section has none so far.
  std::string printedPageLabelFor(uint16_t page) const;
  // What the status bar shows for the current page, recorded in progress.bin for readers of the
  // file that cannot lay the book out (EpubProgressRecord::Shown).
  EpubProgressRecord::Shown shownProgress() const;
  // Re-reads the recorded page counts of the current chapter's other spine items from the book's
  // SpinePageIndex on SD. Runs when chapterSpanSpine_ is not the current spine: -1 after a section
  // is created (spine change, settings change, cache clear) or a Background-B build completes,
  // i.e. whenever a sibling's count may have been recorded. Page turns within a spine cost nothing.
  void refreshChapterSpan() const;
  // The run of spine items making up TOC entry `toc`'s chapter. first == last means the counter
  // stays per spine. A function of the book alone, so it is resolved once per chapter entered.
  struct ChapterBounds {
    int toc = -2;  // -2: nothing resolved yet (-1 is a real answer: no TOC entry)
    int first = -1;
    int last = -1;
  };
  ChapterBounds resolveChapterBounds(int toc) const;
  // Longer runs are a coarse TOC (parts, not chapters): the counter stays per spine rather than
  // walk the spine table that far.
  static constexpr int MAX_CHAPTER_SPAN_FILES = 256;
  mutable ChapterBounds chapterBounds_;
  mutable ChapterPageSpan chapterSpan_;
  mutable int chapterSpanSpine_ = -1;
  // What the counter last showed, so a completed look-ahead build can tell whether it changed it.
  mutable int lastStatusBarChapterTotal_ = -1;
  mutable bool lastStatusBarChapterApprox_ = false;
  // Debug overlay: draws the background-work indicators (A: '.'/'x', B: section build %)
  // in a status-bar corner. Compiled to a no-op unless DEBUG_BACKGROUND_WORK is set.
  void renderBackgroundDebugOverlay() const;
  // Snapshot of the three status-bar signals that can change while a page is otherwise idle.
  // Compared in shouldSkipPeriodicUpdate() to suppress no-op minute-tick re-renders that on
  // X3 panels accumulate visible speckle via repeated no-diff FAST refreshes.
  mutable int lastStatusBarPage = -1;
  mutable int lastStatusBarBattery = -1;
  mutable int lastStatusBarClockMinute = -1;
  bool maybeRestartForFragmentedHeap(uint32_t freeHeap, uint32_t contigHeap);
  // What a restart to the reader must save first, since it skips onExit: the position, when it is authoritative,
  // and the link back stack.
  void saveStateForRestart();
  // A layout change mid-session (a per-book override, the memory ladder's fewer families): the section goes, and
  // the next render builds it again at the paragraph (or page) on screen.
  void resetSectionKeepingPosition();
  void saveProgress(int spineIndex, int currentPage, int pageCount,
                    std::optional<uint16_t> paragraphIndex = std::nullopt,
                    const std::optional<EpubProgressRecord::Shown>& shown = std::nullopt);
  // Writes the canonical EPUB progress.bin record (EpubProgressRecord): spine(2) + page(2) +
  // pageCount(2) + percent(1), plus paragraph(2) when one is given, plus what the status bar
  // showed when that is given. Used by the per-page saveProgress() and by transient writers (sync
  // restore, bookmark jump) so the on-disk format stays consistent regardless of caller. Static
  // (and shared across the split EpubReaderActivity/EpubReaderSync translation units) since it
  // needs no instance state.
  static bool writeReaderProgressCache(const std::string& cachePath, int spineIndex, int currentPage, int pageCount,
                                       uint8_t percent, std::optional<uint16_t> paragraphIndex = std::nullopt,
                                       const std::optional<EpubProgressRecord::Shown>& shown = std::nullopt);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  // The spine and the position within it that `percent` (0-100) of the book falls on. False
  // when the book has no size or spines to measure.
  bool resolvePercent(int percent, int& targetSpineIndex, float& spineProgress) const;
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);

  // Open word selection over the current page, or the dictionary picker when no
  // dictionary is configured. Both suspend background work for their duration.
  void openDictionary();

  // Quiesce background work and hold it quiesced until resumeBackgroundWork(),
  // so a lookup does not compete with the look-ahead for a heap that is already
  // fragmented by the time anyone reaches for a dictionary.
  //
  // Releases the lent 48 KB region and the build arena inside it, aborts a
  // still-live look-ahead build, and discards the pre-rendered next page. Then
  // latches, so nothing re-arms while the overlay is up.
  //
  // Deliberately does NOT discard a look-ahead build that already finished.
  // Section building is designed to resume from previous work: an aborted build
  // keeps its completed extraction so the retry skips the inflate, and a
  // finished build's real product is the cache file on the card, not the
  // in-memory Section (which is a page-offset LUT worth a few hundred bytes).
  // The look-ahead's own Probe state makes the point -- it drops the Section as
  // soon as loadSectionFile() finds the file. Throwing it away here would free
  // nothing worth having and cost the next visit a needless re-open.
  void suspendBackgroundWork();
  void resumeBackgroundWork();
  void launchKOReaderSync(SyncLaunchMode mode = SyncLaunchMode::COMPARE,
                          const SyncPositionOverride* positionOverride = nullptr,
                          const SyncPostAction& postAction = {});
  // BookFinished::launchFinishedBookFlow callback trampoline (plain fn ptr + ctx, per the
  // project's std::function-avoidance convention): pushes finishedBookSync{SpineIndex,Page,
  // PageCount}_ (stashed by the three call sites right before launchFinishedBookFlow, from the
  // same values they write to progress.bin) to the KOReader sync server, landing on whichever
  // destination the finished-book flow resolved (the toggle composes with Go Home / Open Next /
  // Search OPDS rather than replacing them, so `bookPath` may already be the moved-to-/COMPLETED
  // path and `postAction`/`target` may point at the next book or an OPDS author search).
  // Returns false when it cannot run (no credentials, or no live Epub to read a position from),
  // which tells launchFinishedBookFlow to perform the picked action itself rather than leave the
  // user on a screen where nothing happened.
  static bool onFinishedBookSyncRequested(void* ctx, const std::string& bookPath, KOReaderSyncPostAction postAction,
                                          const std::string& target);
  // Reader-close auto-push gate. Returns true if AUTO_PUSH was launched (the caller
  // must not perform its own exit — the sync activity will route to home on completion).
  // Returns false when any of the gates fails (setting off, no credentials, fewer pages read
  // this session than SETTINGS.koSyncMinSessionPages),
  // letting the caller take its normal exit path.
  bool tryAutoPushOnClose();
  // Consume a persisted standalone KOReader sync session for this EPUB. Remote
  // apply writes the mapped reopen position into progress.bin before the normal
  // reader startup path reads it. Upload-complete leaves the existing local
  // progress.bin untouched and simply clears the pending session marker.
  void applyPendingSyncSession();
  // Consume a persisted bookmark-jump request (from GlobalBookmarksActivity) for
  // this book. Rewrites progress.bin to the bookmarked position before the normal
  // reader startup path reads it.
  void applyPendingBookmarkJump();
#if SERIAL_TEST_API
  // Opens at APP_STATE.pendingPercentJump (set by the serial CMD:ACT:READER:<path>@<n>%).
  void applyPendingPercentJump();
#endif
  void applyOrientation(uint8_t orientation);
  void applyTextDarkness(uint8_t textDarkness);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void stopAutomaticPageTurn();
  void applyBookReaderOverrides(int8_t embeddedStyleOverride, int8_t imageRenderingOverride, int8_t fontFamilyOverride,
                                const std::string& sdFontFamilyOverride, int8_t fontSizeOverride,
                                bool bionicReadingOverride, int8_t paragraphAlignmentOverride);
  // Wider variant that also covers AA, hyphenation and guide dots. Used by
  // QuickOverridesActivity; the narrower overload above funnels through here,
  // preserving the AA/hyphenation/guide-dots values currently held on this activity.
  void applyBookReaderOverrides(int8_t embeddedStyleOverride, int8_t imageRenderingOverride, int8_t fontFamilyOverride,
                                const std::string& sdFontFamilyOverride, int8_t fontSizeOverride,
                                int8_t bionicReadingOverride, int8_t paragraphAlignmentOverride,
                                int8_t textAntiAliasingOverride, int8_t hyphenationOverride,
                                int8_t fontSizeNormalizationOverride, int8_t guideDotsOverride,
                                int8_t inlineFootnotePreviewsOverride);
  void openReaderMenu();
  void openQuickOverrides();
  bool getEffectiveEmbeddedStyle() const;
  bool getEffectiveBionicReading() const;
  uint8_t getEffectiveImageRendering() const;
  uint8_t getEffectiveParagraphAlignment() const;
  bool getEffectiveTextAntiAliasing() const;
  bool getEffectiveHyphenation() const;
  bool getEffectiveFontSizeNormalization() const;
  bool getEffectiveGuideDots() const;
  bool getEffectiveInlineFootnotePreviews() const;
  int getEffectiveReaderFontId() const;
  // getEffectiveReaderFontId(), and the family it resolved to (a built-in's label or an SD family
  // name) when `familyOut` is given.
  int resolveReaderFont(std::string* familyOut) const;
  float getEffectiveReaderLineCompression() const;
  // Re-point navTarget at the page currently displayed. Must be called after every move of
  // section->currentPage that stays inside the loaded section — see the definition for why.
  void anchorNavTargetToCurrentPage();
  bool stepPageState(bool isForwardTurn);
  bool stepPageStateLocked(bool isForwardTurn);
  void pageTurn(bool isForwardTurn);
  // Jump `count` pages (the 10-page double-click action): steps the page state under one lock
  // and makes pageTurn()'s hand-off -- drops the old page's deferred AA and pre-render -- before
  // requesting the render. Stops early at a chapter boundary.
  void jumpPages(bool isForwardTurn, int count);
#if ENABLE_BENCHMARKS
  void runRenderBenchmark();
  std::string buildRenderBenchmarkReport(const LastRenderStats& startSnapshot, const BenchmarkAggregate& aggregate,
                                         int forwardTurns, unsigned long forwardMs, int backwardTurns,
                                         unsigned long backwardMs) const;
#endif  // ENABLE_BENCHMARKS

  // Footnote navigation.
  //
  // savePosition is TRUE for every link the reader follows, footnote or not. It was briefly
  // conditional on the link being a real note -- the reasoning being that a contents link is
  // navigation rather than a detour, so it should not push a position to return to. That is
  // wrong about the consequence: the saved position is the ONLY thing Back has to return to, so
  // a link that saves nothing leaves footnoteDepth at 0 and Back closes the book. Device log
  // 2026-09-09: a note reference jumped to the notes chapter and Back went to the home screen.
  //
  // The two kinds are still told apart, but only where the distinction is harmless -- the
  // footnote list groups them (see pageLinkInfoForCurrentPage). Following one always leaves a
  // way back.
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();
  // A followed link's anchor has resolved to `page` of the current spine: if that is where an
  // earlier link was followed from, unwinds the Back stack below it (EpubLinkBackStack::returnDepth).
  // Called with the render lock held, wherever an Anchor target resolves.
  void settleFollowedLink(int page);
  // Persistence of savedPositions, in linkstack.bin beside progress.bin (EpubLinkBackStack).
  // Loaded on open; written on exit (and before a heap-recovery reboot, which skips onExit) when
  // it changed; deleted at the first change after it was loaded. See linkStackOnDisk_.
  void loadLinkBackStack();
  void saveLinkBackStack();
  void noteLinkBackStackChanged();

  // Find the internal links of `page` among the words it draws and publish a tap target for each.
  // Values are indices into currentPageFootnotes, which must already hold this page's links.
  //
  // Called from all three places a page reaches the panel -- the full render, the pre-rendered
  // fast display, and a page drawn from an in-progress build -- because the targets must
  // describe what is on screen and each of those paths shows a different Page object. Publishing
  // from only one of them is the carousel bug again.
  //
  // The parser attaches a link to a page by word COUNT and keeps only its display text and href
  // — where the text landed on screen is never recorded, and the Page object does not outlive the
  // render (renderContents takes it by value), so this cannot be deferred to the tap. Hence a
  // scan here, at each moment a page goes on screen.
  //
  // Cheap by construction: the walk compares stored word text, and only a word that MATCHES
  // costs a wordBox() — which itself reads a precomputed x from the block rather than laying
  // anything out. Nothing is allocated.
  void publishPageLinkTargets(const Page& page, int marginLeft, int contentTop) const;

  // A tap on one of those targets, resolved in loop(). Returns true when it claimed the tap, so
  // the reader stops before the page-turn zones -- a marker can sit anywhere on the page,
  // including inside one.
  bool handleLinkTouch();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

  // Hands Background-B's borrow back before a child activity (the reader menu and everything
  // reachable from it) is pushed on top.
  //
  // While a child is on top the stack keeps this object alive but stops calling loop(), so B
  // makes no progress — yet it still holds the display's secondary framebuffer as its build
  // arena. With no differential baseline the driver promotes every FAST refresh to HALF
  // (FreeInkDisplay::resolveRefreshMode), so each of the child's redraws costs ~1700 ms instead
  // of ~500. Measured X4 2026-08-19: four consecutive menu redraws at 1755 ms each, back to
  // 476 ms on the first refresh after the borrow was returned.
  //
  // Nothing is lost. This is the same endBackgroundBorrow() the first render after the overlay
  // closes would call anyway (via recoverSecondaryBufferIfNeeded), only earlier — and it was
  // frozen for the whole overlay regardless. A build still in flight is discarded and re-probed
  // exactly as it would have been; a COMPLETED one survives for buildSection() to adopt, which
  // is why this is not resetBackgroundBuild().
  //
  // Background-C is deliberately left alone: it builds the section the reader is waiting on, the
  // buffer is released (not borrowed) to give that build headroom, and reclaiming it here would
  // starve a build the user is actively waiting for. That case still degrades refreshes, and the
  // DISP log now says so out loud.
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) override;
  bool isReaderActivity() const override { return true; }
  // Polled by main.cpp's loop without the render lock; see refreshLoopHints().
  bool preventAutoSleep() override;
  // Auto page turn is reading with no hands on the device: no input arrives, so without this the
  // sleep timeout fires mid-book (issue #293). keepAwake(), not preventAutoSleep(): the pages turn
  // every 5-60 s and the loop should still light-sleep between them.
  bool keepAwake() override { return automaticPageTurnActive; }
  // Hold full speed while a section build is in flight. A build only ever runs during reader
  // idle, so main.cpp's inactivity governor has passed IDLE_DOWNCLOCK_MS and drops the CPU
  // to 10 MHz between slices — the per-slice HalPowerManager::Lock then raises it right back,
  // and the loop spent a measured ~30 clock transitions per second bouncing between the two
  // (device trace 2026-08-07), with the build running at roughly a 50% duty cycle because every
  // 40 ms slice was followed by a 50 ms idle delay at 10 MHz. Declaring the work makes the loop
  // stop calling this state idle at all: slices run back to back, the build finishes sooner, and
  // Background-B hands the borrowed framebuffer back that much earlier. Same failure and same
  // remedy as HomeActivity's cover decoding (see its skipLoopDelay). Polled by main.cpp's loop
  // without the render lock; see refreshLoopHints().
  bool skipLoopDelay() override;
  // A pending pre-render leaves the *next* page in the frame buffer; redraw the current page
  // so a screenshot (or any raw frame-buffer capture) matches what the user sees.
  void prepareFramebufferForCapture() override { restoreCurrentPageToBufferIfPreRendered(); }
  // " book=<path> spine=<i>/<count> page=<n>/<count> font=<family> font_pt=<size>", then with a
  // section loaded " chapter=<toc title> chapter_page=<n>/<total>[~] percent=<book %>" (1-based;
  // 0/0 while no section is loaded; ~ = the total is an estimate).
  void appendDebugState(std::string& out) const override;
#if SERIAL_TEST_API
  bool hasPendingScreenWork() override;
  bool hasPendingBackgroundWork() override;
#endif
  bool shouldSkipPeriodicUpdate() const override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;
  bool handleForcedRefresh(HalDisplay::RefreshMode mode) override;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
};

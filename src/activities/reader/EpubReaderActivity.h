#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <atomic>

#include "BookmarkStore.h"
#include "EpubReaderMenuActivity.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"

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

  // Encodes pending navigation intent — where to land once the target section is loaded.
  // Replaces the scattered nextPageNumber / pendingTocIndex / pendingAnchor /
  // cachedSpineIndex / cachedChapterTotalPageCount / pendingPercent* / pendingParagraph* fields.
  struct NavigationTarget {
    enum class Kind : uint8_t {
      Page,       // go to page n (0-based)
      LastPage,   // go to last page of section (was UINT16_MAX sentinel)
      Anchor,     // href fragment (e.g. "note1")
      TocIndex,   // TOC entry index
      Percent,    // normalised 0.0–1.0 within spine
      Paragraph,  // KOReader paragraph LUT index
      ListItem,   // KOReader li-anchored LUT index
    };
    Kind kind = Kind::Page;
    union {
      int page;             // Kind::Page
      int tocIndex;         // Kind::TocIndex
      float spineProgress;  // Kind::Percent
      uint16_t lutIndex;    // Kind::Paragraph / Kind::ListItem
    };
    std::string anchorStr;  // Kind::Anchor; empty for all others
    // Cross-font rescaling: page count of this spine at save time.
    // Non-zero only for Kind::Page when loaded from progress.bin or written during reflow.
    int cachedPageCount = 0;
    int cachedSpineIdx = 0;

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
    static NavigationTarget makeAnchor(std::string a) {
      NavigationTarget t;
      t.kind = Kind::Anchor;
      t.page = 0;
      t.anchorStr = std::move(a);
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
    static NavigationTarget makeParagraph(uint16_t i) {
      NavigationTarget t;
      t.kind = Kind::Paragraph;
      t.lutIndex = i;
      return t;
    }
    static NavigationTarget makeListItem(uint16_t i) {
      NavigationTarget t;
      t.kind = Kind::ListItem;
      t.lutIndex = i;
      return t;
    }

    // Resolves the target into section.currentPage. Must be called on the render task
    // after the section has been loaded (pageCount is known).
    void resolveInto(Section& section, int spineIndex) const;
  };

  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  NavigationTarget navTarget;
  int pagesUntilFullRefresh = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  bool pendingHalfRefreshAfterImagePage = false;
  // When true, large images on the current page are decoded instead of shown as placeholders.
  // Reset to false on every page turn so the next image page starts with a placeholder again.
  bool forceLoadLargeImages = false;
  // Set after each render: true if the current page contains at least one placeholder image.
  bool pageHasPlaceholders = false;
  // Temporary AA suspension when BW snapshot allocation fails under memory pressure.
  // Automatically lifted once heap recovers above hysteresis thresholds.
  bool antiAliasingSuspendedLowMemory = false;
  bool showTruncatedSectionHintThisRender = false;
  uint8_t truncatedSectionHintRendersRemaining = 0;
  int lastWarnedTruncatedSpineIndex = -1;
  struct RenderPhaseStats {
    unsigned long prewarmMs = 0UL;
    unsigned long bwRenderMs = 0UL;
    unsigned long displayMs = 0UL;
    unsigned long bwStoreMs = 0UL;
    unsigned long grayLsbMs = 0UL;
    unsigned long grayMsbMs = 0UL;
    unsigned long grayDisplayMs = 0UL;
    unsigned long bwRestoreMs = 0UL;
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
  LastRenderStats lastRenderStats;
  // Pre-rendered next page: frame buffer holds page content (no status bar) ready to display.
  // Set by the pre-render pass in render(); consumed and cleared by the fast path in pageTurn().
  // Invalidated (ready=false) on any navigation that is not a simple forward page turn.
  struct PreRenderedPage {
    bool ready = false;
    int spineIndex = -1;
    int pageIndex = -1;
  };
  PreRenderedPage preRenderedPage;
  // Set by render() after a normal page render to request a pre-render of the next page.
  bool pendingPreRender = false;
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
  ReaderUtils::InputDrainGuard inputDrainGuard;
  bool automaticPageTurnActive = false;
  // Pages turned in the current reader session. Used to gate auto-push-on-close: a brief
  // inspection of a book should not trigger a network round-trip. Reset on every reader
  // entry; not persisted, since "session" means the lifetime of this activity instance.
  int sessionPagesAdvanced = 0;
  // -1 means use global SETTINGS value.
  int8_t bookEmbeddedStyleOverride = -1;
  int8_t bookImageRenderingOverride = -1;
  int8_t bookFontFamilyOverride = -1;
  std::string bookSdFontFamilyOverride;
  int8_t bookFontSizeOverride = -1;
  bool bookBionicReadingOverride = false;
  int8_t bookParagraphAlignmentOverride = -1;

  // Bookmarks (starred pages)
  BookmarkStore bookmarkStore;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  // Renders page content into the frame buffer (prewarm + BW pass) without drawing the status bar
  // or flushing to the display. Used by the pre-render pass so the status bar can be superimposed
  // at display time with live values (clock, battery).
  void renderPageContentOnly(const Page& page, int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                             int orientedMarginLeft);
  // Draws the status bar over the current frame buffer and flushes to the display.
  // Handles the refresh cycle and grayscale AA pass. page must be the same page
  // that was last rendered into the buffer (needed for image AA re-render).
  void displayPreRenderedPage(const Page& page, int orientedMarginTop, int orientedMarginRight,
                              int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  // Snapshot of the three status-bar signals that can change while a page is otherwise idle.
  // Compared in shouldSkipPeriodicUpdate() to suppress no-op minute-tick re-renders that on
  // X3 panels accumulate visible speckle via repeated no-diff FAST refreshes.
  mutable int lastStatusBarPage = -1;
  mutable int lastStatusBarBattery = -1;
  mutable int lastStatusBarClockMinute = -1;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void launchKOReaderSync(SyncLaunchMode mode = SyncLaunchMode::COMPARE);
  // Reader-close auto-push gate. Returns true if AUTO_PUSH was launched (the caller
  // must not perform its own exit — the sync activity will route to home on completion).
  // Returns false when any of the gates fails (setting off, no credentials, < 3 pages),
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
  void applyOrientation(uint8_t orientation);
  void applyTextDarkness(uint8_t textDarkness);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void stopAutomaticPageTurn();
  void applyBookReaderOverrides(int8_t embeddedStyleOverride, int8_t imageRenderingOverride, int8_t fontFamilyOverride,
                                const std::string& sdFontFamilyOverride, int8_t fontSizeOverride,
                                bool bionicReadingOverride, int8_t paragraphAlignmentOverride);
  void openReaderMenu();
  bool getEffectiveEmbeddedStyle() const;
  uint8_t getEffectiveImageRendering() const;
  uint8_t getEffectiveParagraphAlignment() const;
  int getEffectiveReaderFontId() const;
  float getEffectiveReaderLineCompression() const;
  bool stepPageState(bool isForwardTurn);
  void pageTurn(bool isForwardTurn);
  void runRenderBenchmark();
  std::string buildRenderBenchmarkReport(const LastRenderStats& startSnapshot, const BenchmarkAggregate& aggregate,
                                         int forwardTurns, unsigned long forwardMs, int backwardTurns,
                                         unsigned long backwardMs) const;

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  bool shouldSkipPeriodicUpdate() const override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
};

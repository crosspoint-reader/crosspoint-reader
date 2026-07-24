#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <atomic>
#include <optional>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "PerBookReaderSettings.h"
#include "ProgressMapper.h"
#include "ReadingSessionTracker.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"
#include "clippings/ClippingPageTools.h"
#include "clippings/ClippingStore.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool pendingFinishedMoveSyncError = false;
  bool pendingKOReaderCredentialsNotice = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  // "No dictionary set" popup, shown when a lookup is triggered without a configured dictionary.
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool ignoreNextConfirmRelease = false;
  ReaderUtils::HoldGestureState confirmHold;
  ReaderUtils::PageTurnGestureState pageTurnGesture;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  bool bookmarksWritable = false;
  bool pendingBookmarkStorageError = false;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // SETTINGS is temporarily overlaid while this EPUB is active. The global
  // defaults are restored on every exit path, including sleep and sync.
  PerBookReaderSettings globalReaderSettings;
  PerBookReaderSettings bookReaderSettings;
  bool bookSettingsWritable = true;
  uint8_t autoPageTurnSeconds = 0;
  bool pendingBookSettingsSaveError = false;
  bool pendingBookStylesApplyError = false;
  bool pendingExternalCssWarning = false;
  bool pendingCacheClearError = false;
  std::atomic<bool> safeModePromptRequested{false};
  std::atomic<bool> pendingSafeModeFailureNotice{false};
  std::atomic<bool> pendingSafeModePersistence{false};
  bool pendingSafeModeEnabledNotice = false;

  ClippingStore clippingStore;
  enum class ClippingNotice : uint8_t {
    None,
    Saved,
    LimitReached,
    SaveFailed,
    Unavailable,
    NewerFormat,
    JumpUnavailable,
  };
  ClippingNotice pendingClippingNotice = ClippingNotice::None;
  bool pendingClippingHighlightsTruncatedNotice = false;
  std::optional<ClippingJumpResult> initialClippingJump;
  struct PendingClippingJump {
    uint16_t spineIndex = 0;
    uint16_t page = 0;
    uint32_t pageFingerprint = 0;
    int fallbackSpineIndex = 0;
    int fallbackPage = 0;
    int fallbackCachedSpineIndex = 0;
    int fallbackCachedChapterPageCount = 0;
  };
  std::optional<PendingClippingJump> pendingClippingJump;
  ClippingPageTools::HighlightNoticeTracker clippingHighlightNotices;

  BookReadingStats bookReadingStats;
  GlobalReadingStats globalReadingStats;
  // Read trust and write permission are deliberately separate. A pending
  // transaction can make valid statistics read-only; corrupt/newer files are
  // neither writable nor safe to present as real zeroes.
  bool bookReadingStatsTrusted = true;
  bool globalReadingStatsTrusted = true;
  bool bookReadingStatsWritable = true;
  bool globalReadingStatsWritable = true;
  // Prevent a broken/newer stats file or transient SD failure from causing an
  // unbounded completion retry on every loop tick. Leaving the end screen
  // permits one deliberate retry; reopening the book naturally resets it.
  bool completionAttemptBlocked = false;
  bool pendingStatsCompletionError = false;
  ReadingSessionTracker readingSessionTracker;
  uint32_t sessionReadingSeconds = 0;
  // Time-bucket/history changes are accumulated per actually visible page
  // interval and merged only if the session passes the 10-second noise filter.
  BookReadingStats pendingBookReadingSpans;
  GlobalReadingStats pendingGlobalReadingSpans;
  ReadingStatsDateTime activeReadingSpanStartLocalDateTime;
  bool hasActiveReadingSpanStartLocalDateTime = false;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  bool readingSessionCommitted = false;
  bool bookReadingStatsDirty = false;
  bool globalReadingStatsDirty = false;
  // render() runs on the display task; loop()/lifecycle own the tracker and
  // consume this tiny last-event-wins handoff on the main task.
  std::atomic<int8_t> pendingReadingViewSignal{0};  // -1 hidden, +1 visible
  std::atomic<uint32_t> pendingReadingViewAtMs{0};

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick; the blocking extension in render() remains the fallback past the watermark.
  bool partialRebuildStartFailed = false;

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  enum class DebugSectionCacheStatus : uint8_t { Unknown, Miss, Partial, Hit };
  struct DebugIndexMetrics {
    int spineIndex = -1;
    uint32_t openStartedMs = 0;
    uint32_t openStartFreeHeap = 0;
    uint32_t synchronousPages = 0;
    uint32_t backgroundPages = 0;
    uint32_t lastReportedBackgroundPages = 0;
    DebugSectionCacheStatus cacheStatus = DebugSectionCacheStatus::Unknown;
    bool waitingForVisiblePage = false;
    bool includesSwitchWait = false;
  } debugIndexMetrics;

  void debugBeginSectionOpen(bool includesSwitchWait);
  void debugSetSectionCacheStatus(DebugSectionCacheStatus status);
  void debugRecordSectionBuild(uint16_t before, bool background);
  void debugReportVisibleSection();
  void debugReportCompletedBuild() const;
#endif

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  bool renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft, uint32_t* pageFingerprintOut);
  void renderStatusBar() const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void openDictionaryWordSelect();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  bool launchNearbyPositionSync();
  void applyOrientation(uint8_t orientation);
  void openBookReaderSettings();
  void openReadingStats();
  void openClippingSelection();
  void openClippings();
  bool validateClippingJump(const ClippingJumpResult& jump) const;
  void armClippingJump(const ClippingJumpResult& jump);
  bool abortPendingClippingJump();
  // Returns true when an invalid target restored the previous section and this
  // render must restart there. The page fingerprint is intentionally checked
  // later against the exact Page instance that will be rendered.
  bool preparePendingClippingJump();
  bool persistBookReaderSettings();
  bool queueSafeModePromptIfEligible(EpubBuildStatus status);
  void invalidateReaderLayout();
  void applyAutoPageTurnRuntime(uint8_t seconds, bool active);
  void updateAutoPageTurnFromMenu(uint8_t seconds);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  bool addBookmark();
  void updateBookmarkFlag();

  void signalReadingPageVisible();
  void signalReadingPageHidden();
  void consumeReadingViewSignal();
  void stopReadingPage(bool forwardPageTurn, uint32_t nowMs);
  void recordReadingSample(const ReadingSessionSample& sample);
  void commitReadingSession();
  void saveReadingStats();
  bool refreshEstimatedTimeLeft();
  void markBookCompleted();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              PerBookReaderSettings globalReaderSettings, PerBookReaderSettings bookReaderSettings,
                              bool bookSettingsWritable,
                              std::optional<ClippingJumpResult> initialClippingJump = std::nullopt)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        globalReaderSettings(std::move(globalReaderSettings)),
        bookReaderSettings(std::move(bookReaderSettings)),
        bookSettingsWritable(bookSettingsWritable),
        initialClippingJump(std::move(initialClippingJump)) {}
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed only while the incremental builder can make progress. Once
  // the five-page window is full, normal loop delay saves power until the
  // reader advances and opens more work for the builder.
  bool skipLoopDelay() override {
    return section && section->isBuilding() &&
           (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
  }
  bool isReaderActivity() const override { return true; }
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};

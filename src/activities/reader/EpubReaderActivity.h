#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

  // BLE connection state the status bar last rendered with; loop() watches for
  // a flip and redraws the bar so the "BT connecting" placeholder swaps back
  // to the chapter/book title the moment the connection completes (and
  // returns on disconnect) instead of waiting for the next page turn.
  bool statusBarBleConnected = false;

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  // Skip background build ticks below these floors. The parse path grows word
  // vectors of heap strings — throwing allocations that abort() on OOM under
  // -fno-exceptions. The tick is deferrable work: page-turn transients free up
  // between turns and the build resumes. Calibrated BETWEEN the measured states:
  // steady reading with BLE resident runs at ~29.4 KB free / ~16.4 KB largest
  // block (ticks are safe there), while the field crash happened at 34.7 KB free
  // with an ~11 KB largest block.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 26 * 1024;
  // Fragmentation floor for the same gate: free heap says how much memory exists;
  // maxAlloc says whether any single allocation can actually have it.
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 13 * 1024;
  // Gate for a background build tick: true when the heap can take parse allocations.
  // When BLE is what's squeezing the heap, sheds it (the lifecycle then restarts it
  // behind its gates) instead of stalling the build forever below the floors.
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
  // Heap floor for rendering a page at all. Page deserialization (TextBlock word
  // vectors/strings) and glyph caching allocate through throwing paths that abort()
  // on OOM; below this floor render() lends the framebuffer (and the restore path
  // sheds BLE if reallocation fails) before touching the page.
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // Heap floor for entering a section build. The layout code allocates freely (line-break DP
  // arrays sized by word count, CSS rule lookups, glyph buffers) and under -fno-exceptions an
  // OOM there abort()s the firmware instead of failing cleanly -- so a starved heap must be
  // handled *before* the build, not after. Field data: builds succeed at ~46 KB free with BLE
  // resident; abort() observed at ~11 KB free. CSS styling already degrades below 48 KB
  // (MIN_FREE_HEAP_FOR_CSS), so 40 KB trades a few early BLE teardowns for not crashing.
  static constexpr size_t BUILD_MIN_FREE_HEAP = 40 * 1024;
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openReaderMenu();
  void openDictionaryWordSelect();
  bool launchKOReaderSync();
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  bool loadBook() override;
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  void loop() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};

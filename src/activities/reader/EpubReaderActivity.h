#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#ifdef CROSSPOINT_PAGE_CACHE
// Section.h only forward-declares Page; the cache holds one by value-owning pointer.
#include <Epub/Page.h>
#endif

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
#ifdef CROSSPOINT_PAGE_CACHE
  // One-entry deserialized-page cache, filled by the idle prewarm above. That
  // prewarm already reads (spine, currentPage+1) off SD and deserializes it just
  // to scan its glyphs, then throws the Page away -- and the very next forward
  // turn re-opens section.bin, seeks the page LUT and deserializes the same
  // bytes again, on the page-turn critical path. Keeping the Page instead makes
  // the forward turn SD-free. Owned here rather than by Section because Section
  // objects are destroyed and rebuilt constantly, which is exactly what the
  // cache has to outlive. Owned by EpubReaderActivity rather than ReaderActivity
  // because the prewarm that fills it is EPUB-only; the TXT and XTC readers that
  // share the base have no Section and no prewarm.
  //
  // Steady-state cost: ONE retained Page (tens of KB of text blocks), live from
  // the prewarm that fills the entry until the render that consumes it. Never
  // two: ANY key mismatch, page number included, drops the entry on the spot, so
  // the next prewarm can't allocate a second Page beside a resident one -- which
  // is also why a hit-for-its-own-page-later policy is deliberately not offered.
  //
  // A hit requires ALL of: same section generation, same spine, same page
  // number, same pageCount, same isPartial(), and a section that is not
  // building. Invalidation triggers, enumerated:
  //  * Section replaced -- settings change, spine change, orientation change,
  //    footnote navigation (navigateToHref / restoreSavedPosition), page-load
  //    error recovery, percent/TOC/sync/bookmark jumps, auto-page-turn toggle.
  //    Every one funnels through section.reset() (24 call sites) plus the ONE
  //    site that assigns `section` -- renderBook()'s construction path -- which
  //    bumps sectionGeneration and drops the entry. A reset with no reinstall
  //    yet is covered too: takeCachedPage() drops on !section.
  //  * Section re-paginated in place -- only a build does that, so a fill
  //    requires !isBuilding() and a hit re-checks it. A partial's extension
  //    build starting (loop() or renderBook()) invalidates immediately, as does
  //    build progress that re-numbers pages.
  //  * pageCount / isPartial() moving under an otherwise unchanged Section
  //    (a build finalizing) -- both are part of the key.
  // Fill and consume both run under the RenderLock, which is what makes those
  // key fields coherent. The lock covers the KEY only -- the page number handed
  // to takeCachedPage comes from section->currentPage, which pageTurn() moves
  // from the loop task unlocked, so a stale read there can cost a miss and
  // nothing else.
  std::unique_ptr<Page> cachedPage;
  int cachedPageSpine = -1;
  int cachedPageNumber = -1;
  uint32_t cachedPageGeneration = 0;
  uint16_t cachedPagePageCount = 0;
  bool cachedPagePartial = false;
  // Bumped every time `section` is installed, so an entry can never be matched
  // against a different Section object -- a fresh Section can land on the freed
  // address of the old one, so a pointer alone is not a usable key.
  uint32_t sectionGeneration = 0;
  // All three require the RenderLock (see above).
  void storeCachedPage(int pageNumber, std::unique_ptr<Page> page);
  std::unique_ptr<Page> takeCachedPage(int pageNumber);
  void dropCachedPage();
  void onForcedRefreshLocked() override;
#endif
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

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
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

#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#if defined(CROSSPOINT_NEXT_SECTION_PREBUILD) && !defined(CROSSPOINT_BG_BUILD_TASK)
#error "CROSSPOINT_NEXT_SECTION_PREBUILD requires CROSSPOINT_BG_BUILD_TASK: the prebuild runs on that task."
#endif
#if defined(CROSSPOINT_BG_IMAGE_DECODE) && !defined(CROSSPOINT_BG_BUILD_TASK)
#error "CROSSPOINT_BG_IMAGE_DECODE requires CROSSPOINT_BG_BUILD_TASK: it runs as that task's idle work"
#endif

#ifdef CROSSPOINT_BG_BUILD_TASK
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
#ifdef CROSSPOINT_BG_BUILD_TASK
  // X4 Pro (dual-core S3): drive section builds from a dedicated task pinned to
  // core 0 (the Arduino loopTask and the render task both live on core 1)
  // instead of 2-page ticks stolen from loop(). Same RenderLock discipline and
  // tick size as the loop pump — the lock is held per 2-page tick, never across
  // a chapter — but ticks run back-to-back on an otherwise idle core, so a
  // chapter finalizes in seconds of background time instead of tracking reading
  // pace, and the BUILD_WINDOW_AHEAD throttle (which exists to ration loop-task
  // time on a single core) does not apply. Completion/failure are handed back
  // to the loop task via atomics so reposition/reset/requestUpdate keep running
  // in their usual task context. If task creation fails, the loop-tick pump
  // remains as a runtime fallback (gated on bgBuildTaskHandle == nullptr).
  //
  // The idle wait is notification-driven, not a poll: a permanent 25 ms tick
  // would be 40 wakes/s on core 0 for the whole reading session and would hold
  // the chip out of automatic light sleep, which nothing else about reading
  // prevents. See bgBuildTaskLoop() for the two cadences and the arming points.
  TaskHandle_t bgBuildTaskHandle = nullptr;
  std::atomic<bool> bgBuildStop{false};
  std::atomic<bool> bgBuildExited{false};
  std::atomic<bool> bgBuildCompleteNotify{false};
  std::atomic<bool> bgBuildFailedNotify{false};
  static void bgBuildTaskTrampoline(void* param);
  void bgBuildTaskLoop();
  void startBgBuildTask();
  void stopBgBuildTask();
#endif
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // Prebuild (cache pre-load) of the NEXT spine's Section by the background
  // build task while the reader sits near the end of a fully-built chapter, so
  // the forward chapter-boundary turn swaps a ready Section in instead of the
  // synchronous load. CACHED-ONLY: the prebuild never starts a build — a second
  // live build context would share the Epub's single CssParser and the
  // html/.bin.part paths with any build renderBook() starts, both racy.
  // Everything (construction, loadSectionFile, park, discard, consume) runs
  // under the try-acquired RenderLock, so the shared book.bin metadata handle
  // and all reader state are only ever touched serialized. Cache-miss next
  // spines fall back to today's boundary behavior (remembered in
  // prebuildDeclinedSpine so idle iterations don't re-probe SD).
  std::unique_ptr<Section> prebuiltSection;
  int prebuiltSpineIndex = -1;
  int prebuildDeclinedSpine = -1;
  ReaderRenderSpec prebuiltSpec{};
  // Spec snapshot for the task; written by renderBook() under the RenderLock.
  ReaderRenderSpec lastRenderSpec{};
  bool lastRenderSpecValid = false;
  static bool renderSpecEquals(const ReaderRenderSpec& a, const ReaderRenderSpec& b);
  bool prebuildStep();
  // Start prebuilding when the reader is within this many pages of chapter end.
  static constexpr int PREBUILD_NEAR_END_PAGES = 3;
#endif
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // Pre-decode the images on upcoming pages from the background build task's
  // idle work, so the first view of an image page finds a ready .pxc instead of
  // paying a 0.5-3 s decode on the page-turn critical path (today that decode
  // happens inside the render, behind a placeholder pass). One page is examined
  // and at most one image decoded per idle pass; the decode itself runs with
  // the RenderLock RELEASED, which is what the ImageBlock interlock and the
  // cacheOnly decode mode exist to make safe. Returns true when a decode ran.
  bool imageDecodeStep(bool& workPlausible);
  // How far ahead to look. Three pages is roughly the runway a decode needs at
  // reading pace, and keeps the declined mask a byte.
  static constexpr int IMAGE_DECODE_LOOKAHEAD = 3;
  // Cursor state, touched only by the background task. The declined mask
  // records which lookahead offsets have already been examined and have nothing
  // left to decode, so idle passes stop re-reading their pages; it is reset
  // whenever the reader moves (which is also what re-arms the window).
  int imageDecodeSpine = -1;
  int imageDecodeBasePage = -1;
  uint8_t imageDecodeDeclined = 0;
  // Free-heap floor for starting a pre-decode. The PNG decoder object is ~44 KB
  // (JPEG ~20 KB) and the render task may start its own decode, or a build-time
  // image header probe, on the other core while this one runs; leave room for
  // both rather than win a race for the last block. A render-path decode that
  // loses one marks its image failed for the whole session, so this floor is
  // deliberately generous -- pre-decoding is pure opportunism.
  static constexpr size_t IMAGE_DECODE_MIN_FREE_HEAP = 96 * 1024;
  // The decoder object is a single contiguous allocation.
  static constexpr size_t IMAGE_DECODE_MIN_MAX_ALLOC = 48 * 1024;
  // Page origin of the last render, captured under the RenderLock: a
  // pre-decode must place the image exactly where a render would, because both
  // the screen clip and the dither phase depend on absolute position.
  //
  // Implicit invariant, worth stating because it is load-bearing and unenforced:
  // these describe the layout the CURRENT `section` was paginated with. It holds
  // because every path that changes the layout (settings, orientation, margins,
  // status-bar height, auto-turn indicator) resets `section` in the same lock
  // scope, so the pre-decode -- which refuses to run without a section -- can
  // never pair a new page with an old origin. A future re-pagination that
  // changes the origin WITHOUT resetting the section would break it silently:
  // the .pxc files written from here would be dithered and clipped for the old
  // origin, and nothing downstream would notice, because the header check only
  // compares dimensions. Reset lastRenderMarginsValid in any such path.
  int16_t lastRenderMarginLeft = 0;
  int16_t lastRenderMarginTop = 0;
  bool lastRenderMarginsValid = false;
#endif
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

#ifdef CROSSPOINT_BG_BUILD_TASK
  void onEnter() override;
  void onExit() override;
#endif
  void loop() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};

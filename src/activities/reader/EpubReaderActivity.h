#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>

#include <memory>
#include <optional>

#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

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
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
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

  // Next page staged into the framebuffer at the tail of a render, layout cached here. A forward
  // turn then skips the SD load and BW raster and goes straight to flush. Touched only under
  // RenderLock: prerenderNextPage() writes it, pageTurn() reads it.
  struct PreRenderedPage {
    bool ready = false;
    int spineIndex = -1;
    int pageIndex = -1;
    std::unique_ptr<Page> page;  // reused by the consume path's grayscale pass
  };
  PreRenderedPage preRendered;
  // Set by pageTurn()'s fast path; tells render() the framebuffer already holds the next page's
  // BW content, so it only adds the status bar, flushes, and runs grayscale.
  bool useFastDisplay = false;

  // Paint one page to the panel: optional prewarm + BW raster, status bar, flush, grayscale pass.
  // contentAlreadyInFb skips the BW raster (the framebuffer already holds it, a consumed
  // pre-render); the prewarm still runs under AA because the gray pass re-renders the glyphs.
  void paintPage(Page& page, int orientedMarginTop, int orientedMarginLeft, bool contentAlreadyInFb);
  // Rasterize a page's BW content into the framebuffer (prewarm + render), no status bar, no
  // flush. Used by prerenderNextPage to stage the next page while the reader is idle.
  void renderPageContentOnly(const Page& page, int orientedMarginTop, int orientedMarginLeft);
  // Stage the next text page into the framebuffer so the next forward turn is a flush only. No-op
  // when there is no next page, it has images, heap headroom is low, or it is already staged.
  void prerenderNextPage(int orientedMarginTop, int orientedMarginLeft);
  // Drop any staged pre-render. Call on every navigation that is not a simple forward turn.
  void invalidatePreRender();
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void addBookmark();

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
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};

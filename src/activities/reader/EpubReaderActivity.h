#pragma once
#include <Epub.h>
#include <Epub/Section.h>

#include <optional>
#include <vector>

#include "EpubReaderMenuActivity.h"
#include "activities/ActivityWithSubactivity.h"

class EpubReaderActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  // Set when navigating to a TOC entry in a different spine (chapter skip or chapter selector).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::optional<int> pendingTocIndex;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingSubactivityExit = false;  // Defer subactivity exit to avoid use-after-free
  bool pendingGoHome = false;           // Defer go home to avoid race condition with display task
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  // Chapter-level page info aggregated across spine items sharing a TOC entry
  struct SpineSegment {
    int spineIndex;
    int startPage;         // first page in this spine belonging to the chapter
    int pageCount;         // pages in this segment (endPage - startPage)
    int cumulativeOffset;  // total chapter pages before this segment
  };
  struct ChapterPageInfo {
    std::optional<int> tocIndex;
    std::vector<SpineSegment> segments;
    int totalPages = 0;
  };
  ChapterPageInfo chapterPageInfo;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginLeft) const;
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuBack(uint8_t orientation);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  // Build section caches for all spine items in the current TOC chapter and compute page info.
  void ensureChapterCached(uint16_t viewportWidth, uint16_t viewportHeight);
  // Returns the chapter-relative page number for the current position.
  int getChapterRelativePage() const;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&& lock) override;
};

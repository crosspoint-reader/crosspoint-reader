#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "TxtReaderMenuActivity.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  // Offset-based navigation: no whole-file pagination. We track the byte
  // offset of the page currently on screen and the offset where the next
  // page starts (computed as a side-effect of rendering). Previous-page
  // navigation uses a bounded history stack; going deeper than the stack
  // falls back to a coarse backward scan.
  size_t currentOffset = 0;     // byte offset of current page start
  size_t currentEndOffset = 0;  // byte offset where current page ends (= next page start)
  std::vector<size_t> backHistory;
  static constexpr size_t MAX_BACK_HISTORY = 256;

  // Page-count estimate: sampled from the first rendered page so the status
  // bar can show N/M + % without scanning the whole file. Refined lazily.
  size_t estBytesPerPage = 0;

  size_t fileSize = 0;
  int pagesUntilFullRefresh = 0;

  std::vector<std::string> currentPageLines;
  // Parallel to currentPageLines: true if this line ended a source-file line
  // (paragraph break) and therefore should render left-aligned even in
  // justified mode. False for soft-wrapped continuations which can be spread
  // to fill the viewport.
  std::vector<bool> currentPageLineEndsParagraph;
  // Parallel to currentPageLines: true if this line is the first wrapped
  // segment of a source-file line (paragraph start). Used to decide where
  // extraParagraphSpacing applies.
  std::vector<bool> currentPageLineStartsParagraph;
  int maxLinesPerPage = 0;  // Upper bound; actual lines per page is height-driven.
  int viewportWidth = 0;
  int viewportHeight = 0;
  bool initialized = false;
  // True once loadProgress() has run for this open file. Settings changes
  // recompute the layout but must NOT call loadProgress again — that would
  // reset currentOffset to 0 and lose the user's reading position.
  bool progressLoaded = false;

  // Cached settings (used to detect changes between frames)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  uint8_t cachedExtraParagraphSpacing = 0;
  float cachedLineCompression = 1.0f;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;
  int paragraphSpacingPx = 0;

  // Reader-menu-driven options (ephemeral; not persisted to global settings)
  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  uint8_t currentPageTurnOption = 0;  // index into the menu's pageTurnLabels
  uint8_t currentPageJumpOption = 0;  // index into the menu's pageJumpLabels (0 = off)
  bool pendingScreenshot = false;
  // Set when the user picks a percent jump from the menu; consumed in render().
  bool pendingPercentJump = false;
  int pendingJumpPercent = 0;
  // Suppress one frame of input after returning from a sub-activity so the
  // release event that closed the menu doesn't immediately retrigger here.
  bool skipNextButtonCheck = false;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  void recomputeLayout();
  // Returns true if the byte at offset starts a source line (i.e. previous
  // byte is '\n' or offset == 0). Used to decide whether the page's leading
  // line counts as a paragraph start for extra-spacing purposes.
  bool isOffsetAtLineStart(size_t offset) const;
  bool loadPageAtOffset(size_t offset, bool firstLineIsParagraphStart, std::vector<std::string>& outLines,
                        std::vector<bool>* outEndsParagraph, std::vector<bool>* outStartsParagraph, size_t& nextOffset);
  size_t snapToLineStart(size_t offset) const;
  size_t findBackwardPageStart(size_t endOffset) const;
  void saveProgress() const;
  void loadProgress();

  int estimatedTotalPages() const;
  int estimatedCurrentPage() const;

  // Menu-driven actions
  void onReaderMenuConfirm(TxtReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  // Multi-page navigation. Positive delta moves forward, negative backward.
  // Uses the running bytes-per-page estimate to skip without re-paginating.
  void jumpPages(int deltaPages);
  // Translate a 0-100 percent into an absolute byte offset and reset state to
  // render that position on the next frame.
  void jumpToPercent(int percent);

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("TxtReader", renderer, mappedInput), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};

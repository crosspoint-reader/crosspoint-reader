#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
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
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  uint8_t activePageTurnOption = 0;  // Which option index is currently active (0 = off)

  // Adaptive reading-speed constants (Smart auto-page-turn mode).
  static constexpr unsigned long MIN_ADAPT_ELAPSED_MS = 2000UL;  // Ignore turns faster than 2 s (accidental taps)
  static constexpr uint16_t WPM_ADAPT_MIN = 30;                  // Floor to prevent runaway slowdowns
  static constexpr uint16_t WPM_ADAPT_MAX = 1000;                // Ceiling
  // After a backward turn the next N forward turns are partial-page reads (user re-read what was
  // left of the page they returned to), so they are not valid speed samples and must be skipped.
  // Each backward adaptation increments this; each forward turn decrements and skips adaptation.
  uint8_t skipForwardAdaptCount = 0;

  // Word count of the page currently on screen (set in render(), consumed in adaptReadingSpeed()).
  uint16_t currentPageWordCount = 0;

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
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  // Compute autoflip duration from current page's word count and calibrated WPM.
  // Returns 0 if WPM is uncalibrated or word count is 0 (caller should keep previous duration).
  unsigned long smartPageDurationMs(uint16_t wordCount, uint16_t wpm) const;
  // Adapt readingSpeedWpm via EMA when the user manually turns a page in Smart auto-page-turn mode.
  // elapsedMs is the time spent on the page being left (millis() - lastPageTurnTime).
  void adaptReadingSpeed(bool isForwardTurn, unsigned long elapsedMs);

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
};

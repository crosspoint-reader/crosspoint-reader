#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "EpubReaderMenuActivity.h"
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
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  uint8_t activePageTurnOption = 0;  // Which option index is currently active (0 = off)

  // Adaptive reading-speed constants (Smart auto-page-turn mode).
  static constexpr unsigned long MIN_ADAPT_ELAPSED_MS = 2000UL;   // Ignore fwd turns faster than 2 s (accidental taps)
  static constexpr unsigned long MIN_SMART_DURATION_MS = 2000UL;  // Floor for tiny pages (e.g. 1-word chapter titles)
  static constexpr uint16_t WPM_ADAPT_MIN = 30;                   // Floor to prevent runaway slowdowns
  static constexpr unsigned long BACK_SLOWDOWN_WINDOW_MS =
      3000UL;  // Only slow down if back press within 3 s of auto-turn
  // Tracks how many pages behind the furthest-read position the user currently is.
  // Incremented on every manual backward turn; decremented on every manual forward turn.
  // Auto-turn is paused (timer reset) while this is > 0.
  uint8_t skipForwardAdaptCount = 0;
  // True after the first backward turn following an auto-advance. Additional backward turns (user
  // browsing back further) must not trigger extra slowdowns; only the first one counts.
  // Reset to false each time an auto-turn fires so the cycle can repeat.
  bool backwardSlowdownApplied = false;
  // Set when a forward turn is classified as accidental (elapsed < MIN_ADAPT_ELAPSED_MS).
  // The immediately following backward turn is treated as a correction: skipForwardAdaptCount
  // is not incremented and no slowdown is applied. Cleared on any real forward event.
  bool lastForwardWasAccidental = false;

  // Word count of the page currently on screen (set in render(), consumed in adaptReadingSpeed()).
  uint16_t currentPageWordCount = 0;
  // True when readingSpeedWpm was updated in RAM but not yet flushed to flash.
  // Flushed once in onExit() to avoid a flash write on every page turn.
  bool dirtyReadingSpeedWpm = false;

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
  static constexpr uint16_t WPM_ADAPT_MAX = 1000;  // Ceiling; must equal CrossPointSettings::READING_SPEED_WPM_MAX
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
};

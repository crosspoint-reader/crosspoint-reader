/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossVi
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <atomic>
#include <limits>
#include <string>
#include <utility>

#include "BookReadingStats.h"
#include "EndOfBookOptions.h"
#include "GlobalReadingStats.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  uint32_t lastSavedPage = static_cast<uint32_t>(-1);
  uint32_t lastSuccessfullyRenderedPage = std::numeric_limits<uint32_t>::max();
  int pagesUntilFullRefresh = 0;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  BookReadingStats bookReadingStats;
  GlobalReadingStats globalReadingStats;
  bool bookReadingStatsTrusted = true;
  bool globalReadingStatsTrusted = true;
  bool bookReadingStatsWritable = true;
  bool globalReadingStatsWritable = true;
  bool completionAttemptBlocked = false;
  bool pendingStatsCompletionError = false;
  ReadingSessionTracker readingSessionTracker;
  uint32_t sessionReadingSeconds = 0;
  BookReadingStats pendingBookReadingSpans;
  GlobalReadingStats pendingGlobalReadingSpans;
  ReadingStatsDateTime activeReadingSpanStartLocalDateTime;
  bool hasActiveReadingSpanStartLocalDateTime = false;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  bool readingSessionCommitted = false;
  bool bookReadingStatsDirty = false;
  bool globalReadingStatsDirty = false;
  std::atomic<int8_t> pendingReadingViewSignal{0};
  std::atomic<uint32_t> pendingReadingViewAtMs{0};
  bool ignoreNextConfirmRelease = false;
  ReaderUtils::HoldGestureState confirmHold;
  ReaderUtils::PageTurnGestureState pageTurnGesture;
  bool pendingShortcutUnsupportedNotice = false;
  uint8_t autoPageTurnSeconds = 0;
  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0;
  bool pendingScreenshot = false;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  bool renderPage(const std::shared_ptr<Xtc>& book, uint32_t page);
  // Opens chapter selection when the book has chapters (short-press Confirm); no-op otherwise
  void openChapterSelection();
  void openReaderMenu();
  void handleReaderMenuAction(int action);
  void openGoToPage();
  void confirmMarkBookCompleted();
  void renderStatusBarOverlay(const std::shared_ptr<Xtc>& book, uint32_t page,
                              StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo(const std::shared_ptr<Xtc>& book, uint32_t page) const;
  bool saveProgress(const std::shared_ptr<Xtc>& book, uint32_t page) const;
  void loadProgress();
  void openReadingStats();
  void signalReadingPageVisible();
  void signalReadingPageHidden();
  void consumeReadingViewSignal();
  void stopReadingPage(bool forwardPageTurn, uint32_t nowMs);
  void recordReadingSample(const ReadingSessionSample& sample);
  void commitReadingSession();
  void saveReadingStats();
  void markBookCompleted();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc)
      : Activity("XtcReader", renderer, mappedInput), xtc(std::move(xtc)) {}
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }
  ScreenshotInfo getScreenshotInfo() const override;
};

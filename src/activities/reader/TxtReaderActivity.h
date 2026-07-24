#pragma once

#include <Txt.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "GlobalReadingStats.h"
#include "PerBookReaderSettings.h"
#include "ReadingSessionTracker.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"
#include "activities/reader/EpubReaderMenuActivity.h"
#include "clippings/ClippingStore.h"

class Page;

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int lastSavedPage = -1;
  int lastSuccessfullyRenderedPage = -1;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // Streaming text reader - stores file offsets for each page
  // Bounded, no-throw storage: firmware is built with -fno-exceptions, so an
  // allocating std::vector can abort instead of reporting low memory.
  std::unique_ptr<uint32_t[]> pageOffsets;
  size_t pageOffsetCount = 0;
  size_t pageOffsetCapacity = 0;
  std::vector<std::string> currentPageLines;
  std::vector<uint32_t> currentPageLineOffsets;
  int linesPerPage = 0;
  int viewportWidth = 0;
  int cachedLineAdvance = 0;
  std::atomic<bool> initialized{false};
  bool initializationFailed = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  // TXT shares the same versioned, CRC-protected statistics stores as EPUB.
  // Page pace/ETA are deliberately not sampled because TXT pagination changes
  // with layout; elapsed time, sessions and page turns remain well-defined.
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

  PerBookReaderSettings globalReaderSettings;
  PerBookReaderSettings bookReaderSettings;
  bool bookSettingsWritable = true;
  uint8_t autoPageTurnSeconds = 0;
  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0;
  bool ignoreNextConfirmRelease = false;
  ReaderUtils::HoldGestureState confirmHold;
  ReaderUtils::PageTurnGestureState pageTurnGesture;
  bool pendingShortcutUnsupportedNotice = false;
  bool pendingScreenshot = false;
  bool pendingBookSettingsSaveError = false;
  bool pendingCacheClearError = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool bookmarksWritable = true;
  bool currentPageBookmarked = false;
  bool showBookmarkMessage = false;
  bool bookmarkRemoved = false;
  bool pendingBookmarkStorageError = false;
  unsigned long bookmarkMessageTime = 0;
  std::optional<ClippingJumpResult> initialClippingJump;
  ClippingStore clippingStore;
  enum class ClippingNotice : uint8_t { None, Saved, LimitReached, SaveFailed, Unavailable, NewerFormat, JumpUnavailable };
  ClippingNotice pendingClippingNotice = ClippingNotice::None;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0;

  struct TextWordAnchor {
    uint32_t start = 0;
    uint32_t end = 0;
  };

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                        std::vector<uint32_t>* outLineOffsets = nullptr);
  std::unique_ptr<Page> buildInteractivePage(uint16_t page, std::vector<TextWordAnchor>* anchors = nullptr);
  bool buildPageIndex();
  bool appendPageOffset(uint32_t offset);
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  bool saveProgress() const;
  void loadProgress();
  void openReadingStats();
  void openReaderMenu();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openBookReaderSettings();
  bool persistBookReaderSettings();
  void invalidateReaderLayout();
  void applyOrientation(uint8_t orientation);
  void updateAutoPageTurnFromMenu(uint8_t seconds);
  void jumpToPercent(int percent);
  void loadCachedBookmarks();
  bool toggleBookmark();
  void updateCurrentPageBookmarked();
  void jumpToByteOffset(uint32_t byteOffset);
  void openDictionaryWordSelect();
  void openClippingSelection();
  void openClippings();
  bool validateClippingJump(const ClippingJumpResult& jump) const;
  void signalReadingPageVisible();
  void signalReadingPageHidden();
  void consumeReadingViewSignal();
  void stopReadingPage(bool forwardPageTurn, uint32_t nowMs);
  void recordReadingSample(const ReadingSessionSample& sample);
  void commitReadingSession();
  void saveReadingStats();
  void markBookCompleted();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             PerBookReaderSettings globalReaderSettings = {},
                             PerBookReaderSettings bookReaderSettings = {}, bool bookSettingsWritable = true,
                             std::optional<ClippingJumpResult> initialClippingJump = std::nullopt)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        globalReaderSettings(std::move(globalReaderSettings)),
        bookReaderSettings(std::move(bookReaderSettings)),
        bookSettingsWritable(bookSettingsWritable),
        initialClippingJump(std::move(initialClippingJump)) {}
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

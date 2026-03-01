#pragma once

#include <EpdFontFamily.h>
#include <Txt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;
  TaskHandle_t displayTaskHandle = nullptr;
  std::atomic<bool> exitTaskRequested{false};
  std::atomic<bool> taskHasExited{false};
  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // A processed display line: plain text (inline markers stripped for headings),
  // the font style to render it with, and whether it is a horizontal rule.
  struct StyledLine {
    std::string text;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool isHRule = false;
  };

  bool isMarkdown = false;  // true when the open file has a .md extension

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<StyledLine> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  static void taskTrampoline(void* param);
  void displayTaskLoop();
  bool waitForRenderingMutex();
  void renderScreen();
  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<StyledLine>& outLines, size_t& nextOffset);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("TxtReader", renderer, mappedInput), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};

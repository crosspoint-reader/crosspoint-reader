#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
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
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for invalidating saved progress on layout changes
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  float cachedLineCompression = 1.0f;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, std::vector<bool>* outEndsParagraph,
                        size_t& nextOffset);
  size_t snapToLineStart(size_t offset) const;
  size_t findBackwardPageStart(size_t endOffset) const;
  void saveProgress() const;
  void loadProgress();

  int estimatedTotalPages() const;
  int estimatedCurrentPage() const;

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("TxtReader", renderer, mappedInput), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};

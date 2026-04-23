#pragma once

#include <MdParser.h>
#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class MdReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // A single rendered line on screen (after word-wrapping)
  struct RenderedLine {
    std::vector<MdParser::Span> spans;
    int indent = 0;     // left indent in pixels
    bool isHR = false;  // draw as horizontal rule
  };

  // Streaming reader state
  std::vector<size_t> pageOffsets;
  std::vector<uint8_t> pageCodeBlockState;  // 1 if page starts inside a code block
  std::vector<RenderedLine> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  // Indent constants (in pixels)
  static constexpr int LIST_INDENT = 20;
  static constexpr int BLOCKQUOTE_INDENT = 16;
  static constexpr int CODE_INDENT = 8;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, bool startInCodeBlock, std::vector<RenderedLine>& outLines, size_t& nextOffset,
                        bool& endInCodeBlock);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

  // Word-wrap a parsed markdown line into one or more RenderedLines.
  // Returns true if all content was emitted, false if truncated by maxLines.
  bool wordWrapParsedLine(const MdParser::ParsedLine& parsed, int indent, std::vector<RenderedLine>& outLines,
                          int maxLines);

  // Measure total pixel width of a span list
  int measureSpans(const std::vector<MdParser::Span>& spans) const;

 public:
  explicit MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("MdReader", renderer, mappedInput), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
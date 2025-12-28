#pragma once

#include <expat.h>

#include <climits>
#include <functional>
#include <memory>

#include "../ParsedText.h"
#include "../blocks/TextBlock.h"

class Page;
class GfxRenderer;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void(int)> progressFn;  // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  int viewportWidth;
  int viewportHeight;

  void startNewTextBlock(TextBlock::BLOCK_STYLE style);
  void makePages();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(const std::string& filepath, GfxRenderer& renderer, const int fontId,
                                 const float lineCompression, const bool extraParagraphSpacing, const int viewportWidth,
                                 const int viewportHeight,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const std::function<void(int)>& progressFn = nullptr)
      : filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        completePageFn(completePageFn),
        progressFn(progressFn) {}
  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
};

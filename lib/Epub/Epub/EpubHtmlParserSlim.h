#pragma once

#include <expat.h>
#include <limits.h>

#include <functional>

#include "blocks/TextBlock.h"

class Page;
class EpdRenderer;

#define PART_WORD_BUFFER_SIZE 200

class EpubHtmlParserSlim {
  const char* filepath;
  EpdRenderer& renderer;
  std::function<void(Page*)> completePageFn;
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // If we encounter words longer than this, but this is pretty large
  char partWordBuffer[PART_WORD_BUFFER_SIZE] = {};
  int partWordBufferIndex = 0;
  TextBlock* currentTextBlock = nullptr;
  Page* currentPage = nullptr;

  void startNewTextBlock(BLOCK_STYLE style);
  void makePages();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit EpubHtmlParserSlim(const char* filepath, EpdRenderer& renderer,
                              const std::function<void(Page*)>& completePageFn)
      : filepath(filepath), renderer(renderer), completePageFn(completePageFn) {}
  ~EpubHtmlParserSlim() = default;
  bool parseAndBuildPages();
};

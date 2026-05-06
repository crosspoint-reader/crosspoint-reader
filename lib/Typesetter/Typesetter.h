#pragma once

#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/ImageBlock.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class GfxRenderer;

class Typesetter {
  GfxRenderer& renderer;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint16_t viewportWidth;
  uint16_t viewportHeight;

  std::unique_ptr<Page> currentPage;
  int16_t currentPageNextY = 0;
  int completedPageCount = 0;

  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn;

  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;
  int wordsExtractedInBlock = 0;
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;

 public:
  explicit Typesetter(GfxRenderer& renderer, int fontId, float lineCompression, bool extraParagraphSpacing,
                      uint16_t viewportWidth, uint16_t viewportHeight,
                      std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn);

  ~Typesetter() = default;

  void addLineToPage(std::shared_ptr<TextBlock> line);
  void submitParagraph(std::unique_ptr<ParsedText> paragraph);
  void submitImage(std::shared_ptr<ImageBlock> imageBlock, int16_t imageMarginTop, int16_t imageMarginBottom);
  void finish();

  void setXpathParagraphIndex(uint16_t index) { xpathParagraphIndex = index; }
  uint16_t getXpathParagraphIndex() const { return xpathParagraphIndex; }

  void setXpathListItemIndex(uint16_t index) { xpathListItemIndex = index; }
  uint16_t getXpathListItemIndex() const { return xpathListItemIndex; }

  void addPendingFootnote(int wordIndex, const FootnoteEntry& entry) { pendingFootnotes.push_back({wordIndex, entry}); }

  int getWordsExtractedInBlock() const { return wordsExtractedInBlock; }
  void resetWordsExtractedInBlock() { wordsExtractedInBlock = 0; }

  int getCompletedPageCount() const { return completedPageCount; }

  Page* getCurrentPage() { return currentPage.get(); }
  int16_t getCurrentPageNextY() const { return currentPageNextY; }
  uint16_t getViewportWidth() const { return viewportWidth; }
  uint16_t getViewportHeight() const { return viewportHeight; }
};

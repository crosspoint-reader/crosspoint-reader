#include "Typesetter.h"

#include <GfxRenderer.h>
#include <Logging.h>

Typesetter::Typesetter(GfxRenderer& renderer, int fontId, float lineCompression, bool extraParagraphSpacing,
                       uint16_t viewportWidth, uint16_t viewportHeight,
                       std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn)
    : renderer(renderer),
      fontId(fontId),
      lineCompression(lineCompression),
      extraParagraphSpacing(extraParagraphSpacing),
      viewportWidth(viewportWidth),
      viewportHeight(viewportHeight),
      completePageFn(std::move(completePageFn)) {}

void Typesetter::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

void Typesetter::submitParagraph(std::unique_ptr<ParsedText> paragraph) {
  if (!paragraph) {
    LOG_ERR("TYP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  const BlockStyle& blockStyle = paragraph->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  paragraph->layoutAndExtractLines(renderer, fontId, effectiveWidth,
                                   [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });

  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

void Typesetter::submitImage(std::shared_ptr<ImageBlock> imageBlock, int16_t imageMarginTop,
                             int16_t imageMarginBottom) {
  if (!imageBlock) {
    LOG_ERR("TYP", "Null ImageBlock submitted");
    return;
  }

  const int16_t displayWidth = imageBlock->getWidth();
  const int16_t displayHeight = imageBlock->getHeight();

  if (currentPage && !currentPage->elements.empty() &&
      (currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom > viewportHeight)) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  } else if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  currentPageNextY += imageMarginTop;

  int xPos = (viewportWidth - displayWidth) / 2;
  auto pageImage = std::make_shared<PageImage>(imageBlock, xPos, currentPageNextY);
  currentPage->elements.push_back(pageImage);
  currentPageNextY += displayHeight + imageMarginBottom;
}

void Typesetter::finish() {
  if (currentPage) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset();
  }
}

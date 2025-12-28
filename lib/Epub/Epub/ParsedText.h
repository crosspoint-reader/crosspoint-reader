#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::list<std::string> words;
  std::list<EpdFontStyle> wordStyles;
  TextBlock::BLOCK_STYLE style;
  bool extraParagraphSpacing;

  std::vector<size_t> computeLineBreaks(int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths) const;
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const TextBlock::BLOCK_STYLE style, const bool extraParagraphSpacing)
      : style(style), extraParagraphSpacing(extraParagraphSpacing) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontStyle fontStyle);
  void setStyle(const TextBlock::BLOCK_STYLE style) { this->style = style; }
  TextBlock::BLOCK_STYLE getStyle() const { return style; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, int viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};

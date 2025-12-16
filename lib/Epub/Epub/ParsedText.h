#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>

#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::list<std::string> words;
  std::list<EpdFontStyle> wordStyles;
  TextBlock::BLOCK_STYLE style;
  bool extraParagraphSpacing;

 public:
  explicit ParsedText(const TextBlock::BLOCK_STYLE style, const bool extraParagraphSpacing)
      : style(style), extraParagraphSpacing(extraParagraphSpacing) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontStyle fontStyle);
  void setStyle(const TextBlock::BLOCK_STYLE style) { this->style = style; }
  TextBlock::BLOCK_STYLE getStyle() const { return style; }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, int horizontalMargin,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
};

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
  bool hyphenationEnabled;

 public:
  explicit ParsedText(const TextBlock::BLOCK_STYLE style, const bool extraParagraphSpacing,
                      const bool hyphenationEnabled)
      : style(style), extraParagraphSpacing(extraParagraphSpacing), hyphenationEnabled(hyphenationEnabled) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontStyle fontStyle);
  void setStyle(const TextBlock::BLOCK_STYLE style) { this->style = style; }
  TextBlock::BLOCK_STYLE getStyle() const { return style; }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, int horizontalMargin,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
};

#pragma once
#include <list>
#include <string>

#include "blocks/TextBlock.h"

class ParsedText {
  std::list<std::string> words;
  std::list<uint8_t> wordStyles;

  // the style of the block - left, center, right aligned
  TextBlock::BLOCK_STYLE style;

 public:
  explicit ParsedText(const TextBlock::BLOCK_STYLE style) : style(style) {}
  explicit ParsedText(std::list<std::string> words, std::list<uint8_t> word_styles, const TextBlock::BLOCK_STYLE style)
      : words(std::move(words)), wordStyles(std::move(word_styles)), style(style) {}
  ~ParsedText() = default;
  void addWord(std::string word, bool is_bold, bool is_italic);
  void setStyle(const TextBlock::BLOCK_STYLE style) { this->style = style; }
  TextBlock::BLOCK_STYLE getStyle() const { return style; }
  bool isEmpty() const { return words.empty(); }
  std::list<std::shared_ptr<TextBlock>> splitIntoLines(const GfxRenderer& renderer, int fontId, int horizontalMargin);
};

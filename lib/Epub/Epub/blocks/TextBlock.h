#pragma once
#include <list>
#include <string>
#include <vector>

#include "Block.h"

enum SPAN_STYLE : uint8_t {
  BOLD_SPAN = 1,
  ITALIC_SPAN = 2,
};

enum BLOCK_STYLE : uint8_t {
  JUSTIFIED = 0,
  LEFT_ALIGN = 1,
  CENTER_ALIGN = 2,
  RIGHT_ALIGN = 3,
};

// represents a block of words in the html document
class TextBlock final : public Block {
  // pointer to each word
  std::vector<std::string> words;
  // x position of each word
  std::vector<uint16_t> wordXpos;
  // the styles of each word
  std::vector<uint8_t> wordStyles;

  // the style of the block - left, center, right aligned
  BLOCK_STYLE style;

 public:
  explicit TextBlock(const BLOCK_STYLE style) : style(style) {}
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<uint16_t>& word_xpos,
                     // the styles of each word
                     const std::vector<uint8_t>& word_styles, const BLOCK_STYLE style)
      : words(words), wordXpos(word_xpos), wordStyles(word_styles), style(style) {}
  ~TextBlock() override = default;
  void addWord(const std::string& word, bool is_bold, bool is_italic);
  void setStyle(const BLOCK_STYLE style) { this->style = style; }
  BLOCK_STYLE getStyle() const { return style; }
  bool isEmpty() override { return words.empty(); }
  void layout(EpdRenderer& renderer) override {};
  // given a renderer works out where to break the words into lines
  std::list<TextBlock*> splitIntoLines(const EpdRenderer& renderer);
  void render(const EpdRenderer& renderer, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  void serialize(std::ostream& os) const;
  static TextBlock* deserialize(std::istream& is);
};

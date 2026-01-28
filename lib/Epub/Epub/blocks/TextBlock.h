#pragma once
#include <EpdFontFamily.h>
#include <SdFat.h>

#include <list>
#include <memory>
#include <string>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page
class TextBlock final : public Block {
 public:
  enum Style : uint8_t {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
  };

 private:
  std::list<std::string> words;
  std::list<uint16_t> wordXpos;
  std::list<EpdFontFamily::Style> wordStyles;
  std::list<bool> wordUnderlines;  // Track underline per word
  Style style;
  BlockStyle blockStyle;

 public:
  explicit TextBlock(std::list<std::string> words, std::list<uint16_t> word_xpos,
                     std::list<EpdFontFamily::Style> word_styles, const Style style,
                     const BlockStyle& blockStyle = BlockStyle(), std::list<bool> word_underlines = std::list<bool>())
      : words(std::move(words)),
        wordXpos(std::move(word_xpos)),
        wordStyles(std::move(word_styles)),
        wordUnderlines(std::move(word_underlines)),
        style(style),
        blockStyle(blockStyle) {
    // Ensure underlines list matches words list size
    while (this->wordUnderlines.size() < this->words.size()) {
      this->wordUnderlines.push_back(false);
    }
  }
  ~TextBlock() override = default;
  void setStyle(const Style style) { this->style = style; }
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  Style getStyle() const { return style; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  bool isEmpty() override { return words.empty(); }
  void layout(GfxRenderer& renderer) override {};
  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);
};

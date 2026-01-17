#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::list<std::string> words;
  std::list<EpdFontFamily::Style> wordStyles;
  std::list<bool> wordUnderlines;  // Track underline per word
  TextBlock::Style style;
  BlockStyle blockStyle;
  bool extraParagraphSpacing;

  std::vector<size_t> computeLineBreaks(int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths) const;
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const TextBlock::Style style, const bool extraParagraphSpacing,
                      const BlockStyle& blockStyle = BlockStyle())
      : style(style), blockStyle(blockStyle), extraParagraphSpacing(extraParagraphSpacing) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false);
  void setStyle(const TextBlock::Style style) { this->style = style; }
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  TextBlock::Style getStyle() const { return style; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};

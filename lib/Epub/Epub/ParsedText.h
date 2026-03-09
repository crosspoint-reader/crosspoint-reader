#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

enum class WordJoin : uint8_t {
  SPACED = 0,    // Normal: space before, line break allowed
  ATTACHED = 1,  // Attached: no space before, line break NOT allowed (continuation group)
  JOINED = 2,    // Joined: no space before, line break IS allowed (e.g. Thai clusters)
};

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<WordJoin> wordJoins;
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;

  void applyParagraphIndent();
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<WordJoin>& joinsVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<WordJoin>& joinsVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<WordJoin>& joinsVec, const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

  using ClusterBoundaryFn = size_t (*)(const char*, size_t);
  void addSegmentedWord(const std::string& word, EpdFontFamily::Style style, WordJoin firstJoin,
                        ClusterBoundaryFn nextBoundary);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle), extraParagraphSpacing(extraParagraphSpacing), hyphenationEnabled(hyphenationEnabled) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};
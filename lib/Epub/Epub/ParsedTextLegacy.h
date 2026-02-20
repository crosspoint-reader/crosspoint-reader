#pragma once
// *** BENCHMARK ONLY — do not use in production code ***
// This is a verbatim copy of ParsedText as it existed before the Feb-2026 performance
// optimisation.  It is compiled only when ENABLE_PARSEDTEXT_BENCHMARK is defined.
#ifdef ENABLE_PARSEDTEXT_BENCHMARK

#include <EpdFontFamily.h>

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedTextLegacy {
  std::list<std::string> words;
  std::list<EpdFontFamily::Style> wordStyles;
  std::list<bool> wordContinues;  // original: std::list, not vector
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;

  void applyParagraphIndent();
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks,
                            std::vector<bool>* continuesVec = nullptr);
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedTextLegacy(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                             const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled) {}
  ~ParsedTextLegacy() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false,
               bool attachToPrevious = false);
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};

#endif  // ENABLE_PARSEDTEXT_BENCHMARK

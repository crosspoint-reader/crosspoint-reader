#pragma once

#include <EpdFontFamily.h>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous (no space before it)
  std::vector<bool> wordIsFocusSuffix;  // true = token is the regular tail of a focus bold-prefix split
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;

  // Internal line-emit callback used by the .cpp implementations. Public
  // entry points template their caller's lambda into this fn-pointer + ctx
  // shape so no std::function closure ever heap-allocates (see CLAUDE.md
  // §"Template and std::function Bloat").
  using LineSink = void (*)(void* ctx, std::shared_ptr<TextBlock> line);

  void applyParagraphIndent();
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                   LineSink processLine, void* processLineCtx, const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);
  void layoutAndExtractLinesImpl(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth, LineSink processLine,
                                 void* processLineCtx, bool includeLastLine);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }

  // Templated wrapper: the caller's lambda binds to F at the call site with
  // zero overhead, and we hand the impl a non-capturing thunk + opaque ctx.
  // Previously took `const std::function<...>&`, which heap-allocated the
  // closure under libstdc++ and aborted on OOM in tight-heap scenarios.
  template <typename F>
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth, F&& processLine,
                             bool includeLastLine = true) {
    using Fn = std::remove_reference_t<F>;
    LineSink sink = [](void* ctx, std::shared_ptr<TextBlock> line) { (*static_cast<Fn*>(ctx))(std::move(line)); };
    layoutAndExtractLinesImpl(renderer, fontId, viewportWidth, sink, static_cast<void*>(&processLine), includeLastLine);
  }
};
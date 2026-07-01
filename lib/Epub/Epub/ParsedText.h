#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

// Describes the enlarged initial for a chapter's opening paragraph. Computed by the
// parser (which knows line height / font), then passed to layoutAndExtractLines, which
// strips the prefix from the leading words, insets the first `lineSpan` lines by
// `insetWidth`, and attaches the cap to the first emitted line for rendering.
struct DropCapSpec {
  std::string text;            // enlarged prefix to render, e.g. "W" or "“W" when the
                               // line opens with a quote (already NFC-composed)
  EpdFontFamily::Style style;  // style of the cap letter (applied to the whole prefix)
  uint8_t scale;               // integer upscale factor applied to the glyph bitmaps
  uint16_t insetWidth;         // horizontal space reserved to the left of the inset lines
  uint8_t lineSpan;            // number of leading lines that wrap around the cap
};

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous with no break
  std::vector<bool> wordNoSpaceBefore;  // true = may break before token, but no synthetic space when joined
  std::vector<bool> wordIsFocusSuffix;  // true = token is the regular tail of a focus bold-prefix split
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isNaturalAlign;
  bool hasRtlWord;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<bool> reorderedFocusSuffixScratch;
  std::vector<uint16_t> visualOrderScratch;

  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  // Left inset (px) for the line at the given ordinal. Reproduces the first-line
  // text-indent when there is no drop cap; with a drop cap, the first `lineSpan`
  // lines are inset by insetWidth so text wraps around the enlarged initial.
  int lineLeftInset(size_t lineOrdinal, const GfxRenderer& renderer, int fontId) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  // Greedy, ordinal-aware line breaking for drop-cap paragraphs (the DP breaker
  // only varies width on the first line). Splits overflowing words via hyphenation
  // only when allowHyphenation is set.
  std::vector<size_t> computeDropCapLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                               std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                               bool allowHyphenation);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

  // Removes the leading `count` codepoints (the cap prefix) from the front of the word
  // list, popping fully-consumed words and clearing the new first word's continuation.
  void stripLeadingCodepoints(size_t count);

  // Non-owning; valid only for the duration of a layoutAndExtractLines() call.
  const DropCapSpec* dropCap_ = nullptr;
  bool dropCapCandidate_ = false;

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  // Marks this paragraph as the chapter's opening paragraph (eligible for a drop cap).
  void setDropCapCandidate(const bool v) { dropCapCandidate_ = v; }
  bool isDropCapCandidate() const { return dropCapCandidate_; }
  // Reconstructs the paragraph's plain text (words joined with single spaces, except
  // continuation fragments which glue together). Used to detect a body paragraph that
  // merely repeats the chapter title.
  std::string getPlainText() const;
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, const DropCapSpec* dropCap = nullptr);
  // Builds the drop-cap prefix: up to two leading opening-punctuation codepoints (e.g. a
  // quote) followed by exactly one letter, scanning across continuation-joined leading
  // words. Returns the prefix (NFC, ready to render), the cap letter, and its style.
  // False if the leading run has no codepoint to cap.
  bool buildDropCapPrefix(std::string& outText, uint32_t& letterCp, EpdFontFamily::Style& letterStyle) const;
};

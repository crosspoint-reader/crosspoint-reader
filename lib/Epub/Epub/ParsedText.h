#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

// Describes a paragraph's drop cap. layoutAndExtractLines() strips the prefix from
// the leading words, insets the first `lineSpan` lines by `insetWidth`, and attaches
// the cap to the first emitted line for TextBlock::render to draw enlarged.
struct DropCapSpec {
  std::string text;            // enlarged prefix to render, e.g. "W" or "“W" (NFC-composed)
  EpdFontFamily::Style style;  // style of the cap letter (applied to the whole prefix)
  uint8_t scale;               // integer upscale of the glyph bitmaps (1 = use dedicated face)
  uint16_t insetWidth;         // horizontal space reserved to the left of the inset lines
  uint8_t lineSpan;            // number of leading lines that wrap around the cap
};

class ParsedText {
  // words/rubyTexts are std::deque, not std::vector: a paragraph can hold thousands
  // of tokens (CJK splits every character), and a vector grows by reallocating its
  // whole element array into one contiguous block (32 B/std::string -> 64-128 KB at
  // a few thousand tokens). On the ESP32-C3 that single large contiguous request
  // fails under a fragmented, BLE-resident heap and the throwing operator new
  // abort()s the firmware (fresh-open CJK crash). A deque grows in fixed ~512 B nodes
  // (largest contiguous alloc stays ~2 KB regardless of token count), so it never
  // triggers that. The per-token parallel arrays below stay vectors: 1 byte / 1 bit
  // each, they never approach the contiguous-block ceiling.
  std::deque<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous with no break
  std::vector<bool> wordNoSpaceBefore;  // true = may break before token, but no synthetic space when joined
  std::vector<bool> wordIsFocusSuffix;  // true = token is the regular tail of a focus bold-prefix split
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks do not carry any of this metadata.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  std::deque<std::string> rubyTexts;
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

  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  // Left inset for the line at `lineOrdinal`: the drop-cap wrap inset for the first
  // `lineSpan` lines when a drop cap is active, else the normal first-line indent.
  int lineLeftInset(size_t lineOrdinal, const GfxRenderer& renderer, int fontId) const;
  // Greedy line breaker used only for drop-cap paragraphs: the DP breaker can't take a
  // per-line-ordinal width (the inset varies by line), so lay out line-by-line here.
  std::vector<size_t> computeDropCapLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                               std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                               bool allowHyphenation);
  // Remove `count` leading codepoints (the cap prefix) from the leading words, keeping
  // every parallel array in sync so the cap glyph isn't also drawn inline.
  void stripLeadingCodepoints(size_t count);
  // Drop cap for the current layout pass (borrowed, set by layoutAndExtractLines).
  const DropCapSpec* dropCap_ = nullptr;
  bool dropCapCandidate_ = false;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

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

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0);
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // Marks this paragraph as the chapter's opening candidate for the drop cap (set by
  // the parser when the first real body paragraph opens).
  void setDropCapCandidate(bool v) { dropCapCandidate_ = v; }
  bool isDropCapCandidate() const { return dropCapCandidate_; }
  // Builds the drop-cap prefix: up to two leading opening-punctuation codepoints (e.g. a
  // quote) followed by exactly one letter, scanning across continuation-joined leading
  // words. Returns the prefix (NFC, ready to render), the cap letter, and its style.
  // False if the leading run has no codepoint to cap.
  bool buildDropCapPrefix(std::string& outText, uint32_t& letterCp, EpdFontFamily::Style& letterStyle) const;
  // The block's words joined into plain text (a space before each non-glued word). Used to
  // match a body paragraph against the chapter heading for the drop-cap title dedup.
  std::string getPlainText() const;
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true, const DropCapSpec* dropCap = nullptr);
};

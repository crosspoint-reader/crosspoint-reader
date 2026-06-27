#pragma once

#include <EpdFontFamily.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

struct ParsedToken {
  enum Flag : uint8_t {
    Continues = 1U << 0,
    NoSpaceBefore = 1U << 1,
    FocusSuffix = 1U << 2,
  };

  std::string word;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  uint8_t flags = 0;

  bool continues() const { return (flags & Continues) != 0; }
  bool noSpaceBefore() const { return (flags & NoSpaceBefore) != 0; }
  bool isFocusSuffix() const { return (flags & FocusSuffix) != 0; }

  void setContinues(const bool value) { setFlag(Continues, value); }
  void setNoSpaceBefore(const bool value) { setFlag(NoSpaceBefore, value); }
  void setFocusSuffix(const bool value) { setFlag(FocusSuffix, value); }

 private:
  void setFlag(const Flag flag, const bool value) {
    if (value) {
      flags |= flag;
    } else {
      flags &= static_cast<uint8_t>(~flag);
    }
  }
};

class ParsedTokenStore {
  static constexpr size_t kPageBytes = 1024;
  static constexpr size_t kEntriesPerPage = kPageBytes / sizeof(ParsedToken);
  static_assert(kEntriesPerPage > 0, "ParsedToken page must hold at least one token");

  struct Page {
    std::array<ParsedToken, kEntriesPerPage> tokens;
  };

  std::vector<std::unique_ptr<Page>> pages_;
  size_t size_ = 0;

  ParsedToken& tokenAt(size_t index);
  const ParsedToken& tokenAt(size_t index) const;
  void ensureCapacityForOneMore();

 public:
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  ParsedToken& operator[](size_t index) { return tokenAt(index); }
  const ParsedToken& operator[](size_t index) const { return tokenAt(index); }

  void pushBack(ParsedToken token);
  void insert(size_t index, ParsedToken token);
  void erasePrefix(size_t count);
};

class ParsedText {
  ParsedTokenStore tokens;
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
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                            std::vector<bool>& noSpaceBeforeVec, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
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

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return tokens.size(); }
  bool isEmpty() const { return tokens.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};

#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page
class TextBlock final : public Block {
 private:
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;
  std::vector<std::string> wordLinkHrefs;  // transient: not serialized, used during layout only

 public:
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<std::string> word_link_hrefs = {})
      : words(std::move(words)),
        wordXpos(std::move(word_xpos)),
        wordStyles(std::move(word_styles)),
        blockStyle(blockStyle),
        wordLinkHrefs(std::move(word_link_hrefs)) {
    // Normalize wordLinkHrefs: if empty, treat as "no link metadata" and resize to match words
    if (wordLinkHrefs.empty() && !this->words.empty()) {
      this->wordLinkHrefs.resize(this->words.size(), "");
    } else if (!wordLinkHrefs.empty()) {
      // Ensure word-parallel invariant: wordLinkHrefs.size() == words.size()
      assert(this->wordLinkHrefs.size() == this->words.size());
    }
  }
  ~TextBlock() override = default;
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  const std::vector<std::string>& getWords() const { return words; }
  const std::vector<int16_t>& getWordXpos() const { return wordXpos; }
  const std::vector<EpdFontFamily::Style>& getWordStyles() const { return wordStyles; }
  const std::vector<std::string>& getWordLinkHrefs() const { return wordLinkHrefs; }
  bool isEmpty() override { return words.empty(); }
  size_t wordCount() const { return words.size(); }
  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);
};

#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"
#include "Epub/FootnoteEntry.h"

// Represents a line of text on a page.
//
// All per-word data lives in ONE flat heap allocation (the arena) instead of
// six parallel vectors: a resident page holds ~25-30 of these blocks, and the
// vector-of-string layout cost ~250 throwing allocations per page load, which
// was the primary driver of heap fragmentation on the ESP32-C3.
//
// Arena layout, in order (2-byte alignment holds by construction: all 16-bit
// arrays come first and the arena base is allocator-aligned; RISC-V faults on
// unaligned multi-byte access):
//   uint16_t textOff[wordCount]        byte offset of word i's text in text[]
//   int16_t  xpos[wordCount]
//   uint16_t focusSuffixX[wordCount]   present only when focusPresent
//   uint16_t displayOff[wordCount]     present only when displayPresent; NO_DISPLAY = none
//   uint8_t  styles[wordCount]
//   uint8_t  focusBoundary[wordCount]  present only when focusPresent
//   char     text[textBytes]           all words back to back, NUL-terminated
//   char     display[displayBytes]     present only when displayPresent, NUL-terminated
//
// Each word is stored NUL-terminated so render() can hand `text + textOff[i]`
// straight to C APIs (drawText) with no std::string materialization.
//
// Display text: complex-script words (Bengali) are stored a second time in
// the form drawText renders them, as shaped glyph tokens (see
// GfxRenderer::shapeForDisplay), so a page turn draws them without running the
// shaper. wordText() stays the logical text for dictionary lookup and text
// extraction. Absent entirely when no word on the line needs it.
//
// Focus split semantics (unchanged from the vector layout): boundary N > 0
// means the first N bytes of word i render bold, the remainder in the base
// style. N is bounded to 9 codepoints (<= 36 UTF-8 bytes) by the clamp in
// ParsedText::addWord. focusSuffixX is the pre-computed pixel offset from the
// word start to the regular suffix. Both arrays are omitted from the arena
// entirely when no word on the line has a split (zero per-word RAM cost when
// focus reading is disabled).
class TextBlock final : public Block {
 public:
  struct LinkSpan {
    char href[FOOTNOTE_HREF_LEN];
    int16_t x;
    int16_t width;
    int16_t topLift;
  };

 private:
  BlockStyle blockStyle;
  static constexpr uint16_t NO_DISPLAY = 0xFFFF;
  // Serialized flags byte.
  static constexpr uint8_t FLAG_FOCUS = 1;
  static constexpr uint8_t FLAG_DISPLAY = 2;

  uint16_t numWords = 0;
  uint16_t textBytes = 0;     // total size of the text region, including NULs
  uint16_t displayBytes = 0;  // total size of the display region, including NULs
  bool focusPresent = false;
  bool displayPresent = false;
  bool isValid = true;
  // The ONLY allocation: makeUniqueNoThrow, so OOM yields an invalid block
  // instead of abort() (bare new is not nothrow with -fno-exceptions).
  std::unique_ptr<uint8_t[]> arena;
  // Typed views into the arena, bound once after the arena is filled. All
  // 16-bit bases sit at even offsets, so direct dereference is alignment-safe.
  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  const uint16_t* focusSuffixXArr = nullptr;  // null when !focusPresent
  const uint16_t* displayOffArr = nullptr;    // null when !displayPresent
  const uint8_t* stylesArr = nullptr;
  const uint8_t* focusBoundaryArr = nullptr;  // null when !focusPresent
  const char* textArr = nullptr;
  const char* displayArr = nullptr;  // null when !displayPresent
  std::vector<std::string> rubyTexts;
  // Layout-only metadata. ChapterHtmlSlimParser moves it into Page::links
  // immediately; cached TextBlocks therefore keep the same compact format.
  std::vector<LinkSpan> linkSpans;

  TextBlock() = default;  // deserialize() fills the fields directly
  static size_t arenaSize(uint16_t wordCount, bool hasFocus, uint16_t textBytes, bool hasDisplay,
                          uint16_t displayBytes);
  void bindArenaPointers();

 public:
  // Flatten-on-construct: copies the layout-time vectors into the arena; the
  // vectors die with the caller. On arena OOM the block is empty and valid()
  // is false -- callers must check and fail the line instead of using it.
  // displayWords is empty, or sized like words with "" for words that draw as is.
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSuffixX, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<std::string> rubyTexts = {}, std::vector<LinkSpan> linkSpans = {},
                     const std::vector<std::string>& displayWords = {});
  ~TextBlock() override = default;
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  bool isEmpty() override { return numWords == 0; }
  bool valid() const { return isValid; }
  uint16_t wordCount() const { return numWords; }
  // NUL-terminated by construction; safe to pass to C APIs directly.
  const char* wordText(const uint16_t i) const { return textArr + textOffArr[i]; }
  // What render() draws: the stored display form, else the logical text.
  const char* displayText(const uint16_t i) const {
    return displayPresent && displayOffArr[i] != NO_DISPLAY ? displayArr + displayOffArr[i] : wordText(i);
  }
  uint16_t wordTextLen(const uint16_t i) const {
    const uint16_t end = (i + 1 < numWords) ? textOffArr[i + 1] : textBytes;
    return end - textOffArr[i] - 1;  // exclude the NUL
  }
  int16_t wordXpos(const uint16_t i) const { return xposArr[i]; }
  EpdFontFamily::Style wordStyle(const uint16_t i) const { return static_cast<EpdFontFamily::Style>(stylesArr[i]); }
  uint8_t focusBoundary(const uint16_t i) const { return focusPresent ? focusBoundaryArr[i] : 0; }
  uint16_t focusSuffixX(const uint16_t i) const { return focusPresent ? focusSuffixXArr[i] : 0; }
  bool hasRuby() const;
  int getRubyShift(int ascender) const { return hasRuby() ? (ascender / 2) : 0; }
  const std::vector<std::string>& getRubyTexts() const { return rubyTexts; }
  std::vector<LinkSpan> takeLinkSpans() { return std::move(linkSpans); }

  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(HalFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(HalFile& file);
};

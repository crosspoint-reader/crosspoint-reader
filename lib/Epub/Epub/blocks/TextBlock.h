#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class BuildArena;  // lib/Memory -- optional page-scoped storage for the arena bytes

#include "../TokenBoundary.h"
#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page.
//
// All per-word data lives in ONE flat heap allocation (the arena) instead of
// four parallel vectors (words, xpos, styles, sizes). A resident page holds
// ~25-30 of these blocks, and the vector-of-string layout cost ~250 throwing
// allocations per page load -- the primary driver of heap fragmentation on the
// ESP32-C3 (~380 KB heap, no compaction).
//
// Ported from the sibling crosspoint-reader project's PR #2547 ("Flatten
// TextBlock word storage into single allocation"), which flattens the same
// structure but carries focus-reading annotations; adapted here to carry this
// fork's optional per-word inline font-size array instead.
//
// Arena layout, in order (2-byte alignment holds by construction: the two 16-bit
// arrays come first and the arena base is allocator-aligned; RISC-V faults on
// unaligned multi-byte access):
//   uint16_t textOff[wordCount]   byte offset of word i's text within text[]
//   int16_t  xpos[wordCount]
//   uint8_t  styles[wordCount]    low 7 bits EpdFontFamily::Style, bit 7 = continues
//   uint8_t  sizes[wordCount]     present only when sizesPresent
//   char     text[textBytes]      all words back to back, each NUL-terminated
//
// Each word is stored NUL-terminated so render() can hand `text + textOff[i]`
// straight to C APIs (drawText) with no std::string materialization.
//
// Per-word sizes are the inline CSS font-size percent of the block size. The
// array is omitted from the arena entirely when every word is at 100% (the
// overwhelmingly common case), so ordinary lines pay zero per-word RAM or
// section-cache cost.
// The slice of BlockStyle a laid-out line still needs once it is resident.
//
// A TextBlock is the *product* of layout: the parser has already consumed the
// margins, padding, indent and float zones to compute each word's xpos and the
// line's y, and baked the results into the arena. Only the font selection is
// still needed, because rendering resolves glyphs lazily at draw time.
// `alignment` is not read by render() either, but it is cheap and the pipeline
// golden dump reports it, so it stays as layout provenance.
//
// Keeping the full 52-byte BlockStyle here cost ~1.2 KB per resident page
// (~28 lines/page) in fields nothing on the reader path could read --
// getBlockStyle() had no callers outside the test dump. The same fields were
// also 19 dead bytes per line in the section cache, so they are no longer
// serialized either. See BlockStyle for the parse-time-only fields (floatZones,
// fromBrElement, fontResolved) that are deliberately absent.
//
// RTL / CJK note: this slice is not a barrier to either. CJK line breaking happens
// in ParsedText -- a word with CJK characters becomes one token per allowed break
// (CjkLineBreak.h) -- so a CJK line reaches TextBlock already broken and positioned
// like any other; of its token boundaries only the continuation bit is kept.
// For RTL, the direction state the compiled-content design reserves is per-word
// bidiLevel and a base-direction flag on the block (see CompiledContent.h), not
// a BlockStyle field -- BlockStyle has no direction member at all. Mirroring
// margins/padding/floats is an *input* to positioning, done upstream in the
// parser where the full BlockStyle still lives; a resident block only ever holds
// the resulting xpos/y. `alignment` is kept partly because RTL text is
// right-aligned. Nothing here is lost: the dropped fields remain in BlockStyle
// and on disk, so widening this struct later is a local change.
struct RenderStyle {
  float fontSizeMultiplier = 1.0f;
  int32_t headingFontId = 0;  // 0 = body font scaled by fontSizeMultiplier
  CssTextAlign alignment = CssTextAlign::Justify;
};

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD). Layout measures widths
// with these excluded, so they must not reach the glyph stream either; TextBlock strips them
// while filling its arena. Declared here (rather than in ParsedText.cpp, where the layout-side
// helpers live) because both sides of that copy need the same pattern.
inline constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
inline constexpr size_t SOFT_HYPHEN_BYTES = 2;

class TextBlock final : public Block {
 private:
  RenderStyle renderStyle;
  uint16_t numWords = 0;
  uint16_t textBytes = 0;  // total size of the text region, including one NUL per word
  bool sizesPresent = false;
  bool isValid = true;
  // The ONLY allocation. Heap by default (nothrow, so OOM yields an invalid block instead of
  // abort() -- bare new is not nothrow with -fno-exceptions). A section build that lends a
  // scratch region (the borrowed secondary framebuffer) hands it in through the range
  // constructor: the bytes are then bump-allocated in the page's block of that region and are
  // not ours to free -- the parser rewinds the block once the page is serialised. Per line
  // this was the largest of three heap allocations, ~7,000 per book (memory audit 2026-09, R2).
  uint8_t* arena = nullptr;
  bool arenaOwned_ = false;
  // Takes the arena bytes from `scratch` when given and it has room, else the heap. False on OOM.
  bool allocArena(size_t size, BuildArena* scratch);
  // Typed views into the arena, bound once after the arena is filled. All
  // 16-bit bases sit at even offsets, so direct dereference is alignment-safe.
  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  const uint8_t* stylesArr = nullptr;
  const uint8_t* sizesArr = nullptr;  // null when !sizesPresent
  const char* textArr = nullptr;

  // Byte offsets of each arena region from the arena base. Single source of
  // truth for the layout, shared by the fill path, bindArenaPointers() and
  // arenaSize() so the in-RAM and on-disk layouts can never drift apart.
  struct ArenaOffsets {
    size_t xpos, styles, sizes, text;
  };
  static ArenaOffsets arenaOffsets(uint16_t wordCount, bool hasSizes);

  // Process-wide render option (see setGuideDots): set by the reader activity
  // from settings before pages render. Not per-block state -- blocks are cached
  // and shared across renders, and the aid applies uniformly to every line.
  static bool guideDotsEnabled;

  TextBlock() = default;  // deserialize() fills the fields directly
  static size_t arenaSize(uint16_t wordCount, bool hasSizes, uint16_t textBytes);
  void bindArenaPointers();

  // Effective render scale of word i: block multiplier x the word's size percent.
  float wordScale(const uint16_t i) const {
    return sizesPresent ? renderStyle.fontSizeMultiplier * (sizesArr[i] / 100.0f) : renderStyle.fontSizeMultiplier;
  }

 public:
  // Spare high bit of the packed style byte: EpdFontFamily::Style only uses bits 0-6.
  static constexpr uint8_t WORD_CONTINUES_BIT = 0x80;

  // Flatten-on-construct: copies the layout-time vectors into the arena; the
  // vectors die with the caller. An all-100% sizes vector is normalized to "no
  // sizes". On arena OOM the block is empty and valid() is false -- callers must
  // check and drop the line instead of using it.
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<uint8_t> word_sizes = {}, const std::vector<bool>& word_continues = {});

  // Slice of a laid-out block, addressed directly in the caller's storage.
  // Layout produces lines as [first, first + count) windows over the block's word arrays;
  // this lets the arena be filled straight from those arrays, so a line costs ONE allocation
  // (the arena) instead of also heap-copying every word's std::string into a throwaway
  // per-line vector first. `xpos` is the one array layout computes per line rather than per
  // block, so it is passed separately and is indexed from 0, not from `first`.
  // `sizes` may be empty, meaning every word is at 100%.
  struct WordRange {
    const std::vector<std::string>* words = nullptr;
    const std::vector<EpdFontFamily::Style>* styles = nullptr;
    const std::vector<uint8_t>* sizes = nullptr;  // may be null/empty => uniform 100%
    // Layout's per-word boundaries, indexed like `words`; only `continues` is kept (style bit 7).
    // May be null, meaning every word is space-separated.
    const std::vector<TokenBoundary::Boundary>* boundaries = nullptr;
    size_t first = 0;
    size_t count = 0;
  };

  // Range constructor: same product as the vector constructor above, without the intermediate
  // copies. Soft hyphens are stripped during the arena copy (they must not reach the glyph
  // stream, and the measured widths already exclude them), which is what the vector path used
  // a mutable copy of each word for.
  // xpos by const reference, not by value: it is only read (copied into the arena below), and
  // by-value forced the caller to hand over a vector it had just built, which meant one heap
  // allocation per rendered LINE for pure scratch. See ParsedText::extractLine.
  // `scratch`: see the arena member. Null (the default, and the reader-side deserialize path)
  // means the heap.
  TextBlock(const WordRange& range, const std::vector<int16_t>& word_xpos, const BlockStyle& blockStyle,
            BuildArena* scratch = nullptr);
  ~TextBlock() override {
    if (arenaOwned_) delete[] arena;
  }
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  const RenderStyle& getRenderStyle() const { return renderStyle; }
  bool isEmpty() override { return numWords == 0; }
  bool valid() const { return isValid; }
  uint16_t wordCount() const { return numWords; }
  // NUL-terminated by construction; safe to pass to C APIs directly.
  const char* wordText(const uint16_t i) const { return textArr + textOffArr[i]; }
  uint16_t wordTextLen(const uint16_t i) const {
    const uint16_t end = (i + 1 < numWords) ? textOffArr[i + 1] : textBytes;
    return end - textOffArr[i] - 1;  // exclude the NUL
  }
  int16_t wordXpos(const uint16_t i) const { return xposArr[i]; }
  EpdFontFamily::Style wordStyle(const uint16_t i) const {
    return static_cast<EpdFontFamily::Style>(stylesArr[i] & ~WORD_CONTINUES_BIT);
  }
  // True when word i is glued to word i-1 with no space between them: the two halves of a
  // bionic-reading word, an attached punctuation token, a styled run that changes mid-word.
  // Word 0 carries the flag layout gave the line's first token, so it is only meaningful for
  // i > 0. Rides in the spare high bit of the style byte, so it costs no RAM and no cache
  // bytes; a cache written before the bit existed simply reports false everywhere.
  bool wordContinues(const uint16_t i) const { return (stylesArr[i] & WORD_CONTINUES_BIT) != 0; }
  bool hasWordSizes() const { return sizesPresent; }
  uint8_t wordSizePct(const uint16_t i) const { return sizesPresent ? sizesArr[i] : 100; }
  // Diagnostic/test helper: materializes the per-word size vector (empty when uniform).
  std::vector<uint8_t> getWordSizes() const {
    return sizesPresent ? std::vector<uint8_t>(sizesArr, sizesArr + numWords) : std::vector<uint8_t>{};
  }
  // Largest per-word size percent on this line (100 when uniform). The line's
  // vertical advance scales with this so an enlarged inline word doesn't collide
  // with the next line.
  uint8_t maxSizePct() const;

  // Screen box of one word, in the same coordinate space render(fontId, x, y)
  // draws it in. Shares render()'s geometry rather than restating it: the
  // effective font (a heading block draws with headingFontId), the per-word
  // scale, the baseline alignment that shifts smaller words down, and the
  // SUP/SUB offsets all have to agree, or a caller drawing over a word lands
  // beside it. Used by the dictionary's word-selection overlay.
  struct WordBox {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    // Everything else render() needs to draw this word, so a caller repainting
    // over it produces the same glyphs: a heading block draws with its own
    // font, and a word may carry a style and a size scale of its own.
    int fontId = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    float scale = 1.0f;
  };
  WordBox wordBox(const GfxRenderer& renderer, uint16_t i, int fontId, int x, int y) const;

  // Guide dots reading aid: when enabled, render() draws a small dot centered in
  // the empty space between adjacent words of a line -- never before the first
  // word or after the last. Idea from CrossInk (https://github.com/uxjulia/CrossInk),
  // reimplemented clean-room from its user-facing description. Render-time only:
  // word layout is untouched, so the flag is NOT part of the section cache key
  // and toggling it never triggers a section rebuild.
  static void setGuideDots(const bool enabled) { guideDotsEnabled = enabled; }

  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  // `scratch`: the arena bytes come from it when given (a page drawn mid-build takes them from
  // the build's lent region, inside a block the caller opens and closes); null means the heap.
  static std::unique_ptr<TextBlock> deserialize(FsFile& file, BuildArena* scratch = nullptr);
};

#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "TokenBoundary.h"
#include "TypographyProfile.h"
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;
class BuildArena;

class ParsedText {
 public:
  enum class LineProcessResult {
    Accepted,
    RetryWithoutHyphenation,
  };

  // Where the line being laid out sits in its paragraph (linePlace()): set before beforeLine_
  // and processLine run, for the parser's widow/orphan and keep-with-next decisions.
  struct LinePlace {
    bool paragraphStart = false;  // the paragraph's first line (not a continuation pass)
    // Lines from this one to the paragraph's last; 0 when that is not known yet (the pass
    // stops short of the paragraph's end, see includeLastLine).
    uint16_t linesLeft = 0;
  };

 public:
  // Per-word font-size bounds, percent of the block's font size (microreader-style).
  // Inline CSS font-size composes multiplicatively through nested elements; anything
  // outside this range is clamped so a uint8_t always holds the value.
  static constexpr uint8_t MIN_WORD_SIZE_PCT = 30;
  static constexpr uint8_t MAX_WORD_SIZE_PCT = 250;
  static constexpr uint8_t DEFAULT_WORD_SIZE_PCT = 100;

 private:
  std::vector<std::string> words;
  // Per-line x positions, reused across every line this block produces. Deliberately a member and
  // not a local: as a local it was a fresh heap allocation for EVERY rendered line, whose whole
  // life was reserve/fill/hand-over/free -- the line's words were already flattened into
  // TextBlock's single arena, and this handed the x positions over separately. Host profile of
  // one book: 16258 allocations at extractLine against 14459 rendered lines, essentially one
  // each, in the 16-32 byte classes that dominate the allocation count.
  std::vector<int16_t> lineXPosScratch_;
  // Layout scratch kept across paragraphs (the parser reuses one ParsedText, see reset()).
  // Each of these was a per-block local before: one allocation per paragraph apiece, ~30,000
  // per book on the host census once the word vectors stopped regrowing (memory audit
  // 2026-09, R2). clear() keeps their capacity, so a chapter allocates them once.
  std::vector<uint16_t> wordWidths_;
  std::vector<size_t> lineBreakIndices_;
  std::vector<bool> lineEndsWithHyphenatedWord_;
  std::vector<int> splitPrefixWordIndexes_;
  std::vector<bool> splitInsertedHyphen_;
  std::vector<size_t> suffixBreaks_;
  std::vector<bool> suffixLineEndsWithHyphenatedWord_;
  std::vector<int> suffixSplitPrefixWordIndexes_;
  std::vector<bool> suffixSplitInsertedHyphen_;
  std::vector<int> interWordGaps_;
  std::vector<int> lineIndexForWord_;  // computeLineBreaks' DP tables
  std::vector<int> dp_;
  std::vector<size_t> ans_;
  std::string allText_;
  // Where each line's TextBlock takes its bytes: the build's lent region (a page-scoped block
  // the parser opens), or the heap when null. The parser sets it on its main text block (with
  // beforeLine_, see there for why the two go together) and on table cells laid out as a grid
  // (without the hook: the row is placed as a whole inside a block of its own). A cell laid out
  // as a fallback paragraph keeps the heap.
  BuildArena* lineArena_ = nullptr;
  // Called with the line's largest word-size percent just before the line is materialised.
  // The parser uses it to run its page-fit test BEFORE the allocation: a line that does not
  // fit is then allocated from the page it lands on, not from the block of the page it
  // overflowed -- with page-scoped arena blocks, allocating first would leave page N+1 holding
  // bytes that die with page N.
  std::function<void(uint8_t maxSizePct)> beforeLine_;
  std::vector<EpdFontFamily::Style> wordStyles;
  // How each word joins the one before it (TokenBoundary.h): a space, glued, or a breakable CJK gap.
  // In lockstep with `words` through every insert/erase, like the per-word vectors below.
  std::vector<TokenBoundary::Boundary> wordBoundaries;
  // Per-word font size, percent of the block font size (100 = block size), and font slot. Kept in
  // lockstep with `words` through every insert/erase; the sizes are handed to each
  // TextBlock line so inline font-size spans survive into the page cache. The slot indexes
  // fontSlots_: a word in another of the book's families than the block's (Publisher Fonts)
  // is measured and drawn in that family's face, which the parser puts in its slot; slot 0
  // (every word, without families) is the block's font until the parser says otherwise.
  // Per-word slots follow crosspoint-reader PR #3934 (Justin Mitchell), its wordSizeSlots.
  std::vector<TextBlock::WordSize> wordSizes;
  TextBlock::WordFont fontSlots_[TextBlock::kMaxWordFonts] = {};
  uint8_t fontSlotsUsed_ = 0;  // a bit per slot a word of the block has had
  // Where each word begins in the chapter's visible text (VisibleText.h's rule), in lockstep with
  // `words` through every insert/erase. The section cache records the first word of each page from
  // it (KOReader sync, content offsets). 4 B per word; grown with the other word vectors in one
  // step and counted in addWord's heap gate.
  std::vector<uint32_t> wordVisibleOffsets;
  // The first word's offset of the line most recently handed to processLine. Read by the parser's
  // line callbacks, which only get the TextBlock.
  uint32_t lastLineVisibleOffset_ = 0;
  LinePlace linePlace_;
  BlockStyle blockStyle;
  // The book's typography (TypographyProfile.h): Latin rules until the parser sets the book's.
  TypographyProfile typography_;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool bionicReadingEnabled;
  bool isContinuation_ = false;  ///< true after an intermediate flush; suppresses re-applying paragraph indent
  // Set (and never cleared) when addWord could not grow the word vectors within the largest
  // free heap block: the word was dropped and the parser must abort the parse at its next
  // layout gate. std::vector growth cannot fail gracefully under -fno-exceptions, so the
  // check has to happen before the reserve.
  bool wordGrowthRefused_ = false;
  size_t bionicTransformedUpTo_ = 0;  ///< words[0..bionicTransformedUpTo_) have already been bionic-transformed

  void applyParagraphIndent(const GfxRenderer& renderer, int fontId);
  void applyBionicReadingTransform();
  // Effective measurement scale of words[i]: the block-level multiplier (a word font's own scale for a word in
  // another family) combined with the word's own inline size percentage.
  float wordScale(const size_t i) const {
    const TextBlock::WordFont& font = fontSlots_[wordSizes[i].fontSlot];
    return (font.fontId != 0 ? font.scale : blockStyle.fontSizeMultiplier) * (wordSizes[i].pct / 100.0f);
  }
  // The font words[i] is measured in: its slot's, else the block's (`fontId`).
  int wordFontId(const size_t i, const int fontId) const {
    const int32_t slotFont = fontSlots_[wordSizes[i].fontSlot].fontId;
    return slotFont != 0 ? slotFont : fontId;
  }
  // The space before words[j] (j > 0), unscaled, in the font of the word before it; and the kerning across a glued
  // boundary there, none between words in two fonts.
  int spaceBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
  int kernBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
  // Returns the available line width at a given 0-based line index, accounting
  // for any active float zones in blockStyle.  lineHeight==0 is a fast path
  // (no float zones active) that returns pageWidth unchanged.
  int widthForLine(int lineIndex, int lineHeight, int16_t blockStartY, int pageWidth) const;
  // The natural gap before words[j] (j > 0), unscaled: kerning across a glued boundary, nothing at a
  // CJK gap, else the space advance. Every breaker measures gaps through this.
  int naturalGapBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
  // Room in all five per-word vectors for `additional` more tokens; false (and wordGrowthRefused_)
  // when the heap cannot hold the growth.
  bool reserveWords(size_t additional);
  void pushToken(std::string token, EpdFontFamily::Style style, TokenBoundary::Boundary boundary, uint8_t sizePct,
                 uint32_t visibleOffset, uint8_t fontSlot);

  // The three breakers fill `lineBreakIndices` (cleared first) rather than returning a vector,
  // so the caller's scratch member is reused instead of reallocated per paragraph.
  // `boundaries` is always wordBoundaries itself: hyphenateWordAtIndex inserts into the member during
  // the call, and the reference sees the insert. Passing a copy would desynchronize the indices.
  void computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, std::vector<uint16_t>& wordWidths,
                         const std::vector<TokenBoundary::Boundary>& boundaries, int firstLineIndent,
                         int16_t blockStartY, int lineHeight, std::vector<size_t>& lineBreakIndices);
  void computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                   std::vector<uint16_t>& wordWidths,
                                   const std::vector<TokenBoundary::Boundary>& boundaries,
                                   std::vector<bool>& lineEndsWithHyphenatedWord,
                                   std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen,
                                   int firstLineIndent, int16_t blockStartY, int lineHeight,
                                   std::vector<size_t>& lineBreakIndices);
  // Recompute hyphenated breaks for a suffix that starts at startIndex.
  // Used after a single-line retry so later lines keep normal hyphenation.
  void computeHyphenatedLineBreaksFromIndex(const GfxRenderer& renderer, int fontId, int pageWidth,
                                            std::vector<uint16_t>& wordWidths,
                                            const std::vector<TokenBoundary::Boundary>& boundaries, size_t startIndex,
                                            std::vector<bool>& lineEndsWithHyphenatedWord,
                                            std::vector<int>& splitPrefixWordIndexes,
                                            std::vector<bool>& splitInsertedHyphen, int16_t blockStartY, int lineHeight,
                                            int startLineIdx, std::vector<size_t>& lineBreakIndices);
  // Compute exactly one line break without hyphenating words.
  // Used only for the page-boundary retry line.
  size_t computeSingleLineBreakNoHyphen(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        const std::vector<uint16_t>& wordWidths,
                                        const std::vector<TokenBoundary::Boundary>& boundaries, size_t lineStartIndex,
                                        int firstLineIndent, int16_t blockStartY = 0, int lineHeight = 0) const;
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks,
                            bool* outInsertedHyphen = nullptr);
  LineProcessResult extractLine(
      size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
      const std::vector<TokenBoundary::Boundary>& boundaries, const std::vector<size_t>& lineBreakIndices,
      const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
      const GfxRenderer& renderer, int fontId, bool lineEndsWithHyphenatedWord, bool suppressHyphenationRetry,
      int firstLineIndent, int16_t blockStartY = 0, int lineHeight = 0);
  // Fills `out` (cleared first); uses blockStyle.fontSizeMultiplier internally.
  void calculateWordWidths(const GfxRenderer& renderer, int fontId,
                           std::vector<uint16_t>& out);  // uses blockStyle.fontSizeMultiplier internally

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const bool bionicReadingEnabled = false)
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        bionicReadingEnabled(bionicReadingEnabled) {}
  ~ParsedText() = default;

  // Start a new, empty block in this object under `blockStyle`, keeping the word vectors'
  // capacity. The parser reuses one ParsedText across a chapter's paragraphs: a fresh object
  // per paragraph regrew four vectors from 16 to 128 entries each time -- ~16 allocations and
  // ~3 KB of heap traffic per paragraph, 15,000 allocations per book on the host census, the
  // single largest churn of a section build (memory audit 2026-09, R2).
  void reset(const BlockStyle& blockStyle);
  // Give the layout scratch (and the word vectors, when the block is empty) back to the heap.
  // The parser calls this when an incremental build yields a slice: that is the one moment a
  // mid-build page draw can run, and its ~10.5 KB Page sits on the heap next to whatever the
  // parse holds. Keeping ~8 KB of scratch resident across that moment cost the X3 5 KB of
  // minimum free heap (watermark 11.3 KB -> 6.4 KB, device run 7); dropping it at the yield
  // keeps the no-churn behaviour within a slice and the old floor across it.
  void releaseLayoutScratch();

  // Adds a word from the source: a Latin word as one token, a word with CJK characters as one token per
  // break the CJK rules allow (CjkLineBreak.h). attachToPrevious: no whitespace separated it from the
  // previous word in the source.
  // fontSlot: the word's font slot (see setFontSlot), below TextBlock::kMaxWordFonts.
  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint8_t sizePct = DEFAULT_WORD_SIZE_PCT, uint32_t visibleOffset = 0, uint8_t fontSlot = 0);
  // The face the words in `slot` are laid out in; a fontId of 0 (every slot after reset()) is the block's font. The
  // parser fills the slots its block's words use once it knows the block's font (see fontSlotsUsed).
  void setFontSlot(const uint8_t slot, const TextBlock::WordFont& font) {
    if (slot < TextBlock::kMaxWordFonts) fontSlots_[slot] = font;
  }
  // The slots the block's words have been in, a bit per slot.
  uint8_t fontSlotsUsed() const { return fontSlotsUsed_; }
  // True once addWord had to drop a word because the word vectors could not grow (see
  // wordGrowthRefused_). ChapterHtmlSlimParser::ensureHeapForTextLayout turns it into a
  // partial-cache abort.
  bool wordGrowthRefused() const { return wordGrowthRefused_; }
  // If every word shares one non-100% size (a span wrapping the whole paragraph, e.g.
  // Alice's mouse-tale lines), fold that percent into the block-level fontSizeMultiplier
  // and reset the per-word sizes to 100. This routes whole-paragraph spans through the
  // block font-resolution path (size ladder) so they render with a real smaller font
  // instead of per-word glyph scaling. Returns true when a fold happened. Must be called
  // before the first layout pass of the block; callers skip continuations.
  bool foldUniformWordSizes();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  // Survives reset(): it is the book's, not the block's.
  void setTypography(const TypographyProfile& typography) { typography_ = typography; }
  // See lineArena_ / beforeLine_. Both survive reset().
  void setLineArena(BuildArena* arena) { lineArena_ = arena; }
  void setBeforeLineHook(std::function<void(uint8_t maxSizePct)> hook) { beforeLine_ = std::move(hook); }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  uint32_t lastLineVisibleOffset() const { return lastLineVisibleOffset_; }
  const LinePlace& linePlace() const { return linePlace_; }
  std::optional<uint32_t> firstWordVisibleOffset() const {
    if (wordVisibleOffsets.empty()) return std::nullopt;
    return wordVisibleOffsets.front();
  }
  bool isEmpty() const { return words.empty(); }
  bool isContinuation() const { return isContinuation_; }

  // CONSUMES ITS SOURCE by default: every word it lays out is erased from `words` (and the
  // parallel per-word vectors) before returning, so the vector shrinks as the paragraph drains.
  // A caller that needs to lay the SAME text out twice must pass preserveSource.
  //
  // This was previously documented as preserving its source, which was wrong and cost content: the
  // table grid path laid a cell out to discover it needed more lines than the grid can carry, then
  // handed the same ParsedText to emitTableAsParagraphs — by which point every word of that cell,
  // and of every cell laid out before it, had already been erased. The whole cell rendered as
  // nothing. Fixture: test_table_grid_edges.epub ch3.
  //
  // `preserveSource` snapshots the words on entry and restores them on return, so the caller gets
  // back exactly what it passed in. Suppressing the erase alone would not be enough: layout also
  // force-splits any word too wide for the line (inserting a hyphen), and applies the paragraph
  // indent and bionic transform in place. It costs one copy of the word vectors for the duration
  // of the call, so it is opt-in and only the table grid path uses it.
  void layoutAndExtractLines(
      const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
      const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
      bool includeLastLine = true,
      int16_t blockStartY = 0,  // currentPageNextY at call site — needed for float zone geometry
      int lineHeight = 0,       // 0 = no float zones (fast path, existing callers unchanged)
      bool preserveSource = false);
};
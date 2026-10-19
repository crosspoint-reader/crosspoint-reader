#include "ParsedText.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <vector>

#include "CjkLineBreak.h"
#include "CjkPunctuation.h"
#include "Epub/VisibleText.h"
#include "hyphenation/HyphenationCommon.h"
#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Closing punctuation that should not have extra space inserted before it during justification.
// Includes common closing brackets/quotes and sentence-ending marks. En/em dashes
// are also treated as inline separators here to avoid justification stretch
// immediately before them.
bool isClosingPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case '!':
    case '?':
    case ':':
    case ';':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x203A:  // ›
    case 0x2019:  // '  right single quotation mark
    case 0x201D:  // "  right double quotation mark
    case 0x2026:  // … ellipsis
    case 0x2013:  // – en dash
    case 0x2014:  // — em dash
      return true;
    default:
      return false;
  }
}

// SOFT_HYPHEN_UTF8 / SOFT_HYPHEN_BYTES come from TextBlock.h — the arena fill strips the same
// pattern this file measures around, so both sides share one definition.

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false, const float scale = 1.0f) {
  int raw = 0;
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    raw = renderer.getSpaceWidth(fontId, style);
  } else {
    const bool hasSoftHyphen = containsSoftHyphen(word);
    if (!hasSoftHyphen && !appendHyphen) {
      raw = renderer.getTextAdvanceX(fontId, word.c_str(), style);
    } else {
      std::string sanitized = word;
      if (hasSoftHyphen) stripSoftHyphensInPlace(sanitized);
      if (appendHyphen) sanitized.push_back('-');
      raw = renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
    }
  }
  if (scale == 1.0f) return static_cast<uint16_t>(raw);
  return static_cast<uint16_t>(raw * scale + 0.5f);
}

std::string buildLinePreview(const std::vector<std::string>& words,
                             const std::vector<TokenBoundary::Boundary>& boundaries, const size_t start,
                             const size_t endExclusive, const size_t maxLen = 120) {
  // Build a readable line preview while preserving continuation semantics
  // (no synthetic spaces before attached tokens or at CJK gaps).
  std::string preview;
  for (size_t idx = start; idx < endExclusive; ++idx) {
    if (idx > start && idx < boundaries.size() && TokenBoundary::hasSpace(boundaries[idx])) {
      preview.push_back(' ');
    }
    preview += words[idx];
    if (preview.size() >= maxLen) {
      preview.resize(maxLen);
      preview += "...";
      break;
    }
  }
  return preview;
}

constexpr int kBionicReadingMinCodepoints = 4;
constexpr int kBionicReadingMinBoldPrefix = 1;
constexpr int kBionicReadingBoldPrefixNumerator = 1;
constexpr int kBionicReadingBoldPrefixDenominator = 2;

struct TokenSpan {
  size_t start;
  size_t end;
  bool isWord;
};

static int computeBionicBoldPrefixCount(const int codepointCount) {
  return std::max(kBionicReadingMinBoldPrefix,
                  (codepointCount * kBionicReadingBoldPrefixNumerator + kBionicReadingBoldPrefixDenominator - 1) /
                      kBionicReadingBoldPrefixDenominator);
}

static bool isBionicWordCodepoint(const uint32_t cp) {
  if (cp == 0) {
    return false;
  }
  if (utf8IsCombiningMark(cp)) {
    return true;
  }
  return isAlphabetic(cp) || isAsciiDigit(cp) || isApostrophe(cp);
}

// Split a word token into contiguous spans of "word-like" characters and non-word characters.
// This avoids applying bionic bolding to punctuation, digits-only runs, or other separators.
// Only spans marked as word-like are eligible for the bionic prefix transform.
static std::vector<TokenSpan> tokenizeBionicWord(const std::string& word) {
  std::vector<TokenSpan> spans;
  spans.reserve(2);

  const unsigned char* base = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* ptr = base;
  const unsigned char* segmentStart = ptr;
  bool currentIsWord = false;
  bool haveCurrent = false;

  while (true) {
    const unsigned char* cpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) {
      break;
    }

    bool cpIsWord = isBionicWordCodepoint(cp);
    if (!haveCurrent) {
      currentIsWord = cpIsWord;
      haveCurrent = true;
    } else if (!utf8IsCombiningMark(cp) && cpIsWord != currentIsWord) {
      spans.push_back({static_cast<size_t>(segmentStart - base), static_cast<size_t>(cpStart - base), currentIsWord});
      segmentStart = cpStart;
      currentIsWord = cpIsWord;
    }
  }

  if (haveCurrent) {
    spans.push_back({static_cast<size_t>(segmentStart - base), word.size(), currentIsWord});
  }

  return spans;
}

}  // namespace

bool ParsedText::reserveWords(const size_t additional) {
  const size_t requiredSize = words.size() + additional;
  if (words.capacity() >= requiredSize && wordStyles.capacity() >= requiredSize &&
      wordBoundaries.capacity() >= requiredSize && wordSizes.capacity() >= requiredSize &&
      wordVisibleOffsets.capacity() >= requiredSize) {
    return true;
  }
  size_t newCapacity = std::max<size_t>(16, words.capacity());
  while (newCapacity < requiredSize) {
    newCapacity *= 2;
  }
  // The five reserves below are unchecked heap growth (abort() on failure under
  // -fno-exceptions). Require the largest free block to hold all five, plus a header
  // each -- conservative, since they need not share a block, but this only bites when the
  // heap is nearly gone, and then a partial-cache abort beats a crash.
  {
    constexpr size_t ALLOC_HEADER_SLACK = 16;
    const size_t needed =
        newCapacity * (sizeof(std::string) + sizeof(EpdFontFamily::Style) + sizeof(TextBlock::WordSize) +
                       sizeof(uint32_t) + sizeof(TokenBoundary::Boundary)) +
        5 * ALLOC_HEADER_SLACK;
    if (ESP.getMaxAllocHeap() < needed) {
      wordGrowthRefused_ = true;
      return false;
    }
  }
  words.reserve(newCapacity);
  wordStyles.reserve(newCapacity);
  wordBoundaries.reserve(newCapacity);
  wordSizes.reserve(newCapacity);
  wordVisibleOffsets.reserve(newCapacity);
  return true;
}

void ParsedText::pushToken(std::string token, const EpdFontFamily::Style style, const TokenBoundary::Boundary boundary,
                           const uint8_t sizePct, const uint32_t visibleOffset, const uint8_t fontSlot) {
  words.push_back(std::move(token));
  wordStyles.push_back(style);
  wordBoundaries.push_back(boundary);
  wordSizes.push_back({sizePct, fontSlot});
  fontSlotsUsed_ |= static_cast<uint8_t>(1u << fontSlot);
  wordVisibleOffsets.push_back(visibleOffset);
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint8_t sizePct, const uint32_t visibleOffset,
                         uint8_t fontSlot) {
  if (word.empty()) return;
  if (fontSlot >= TextBlock::kMaxWordFonts) fontSlot = 0;
  if (wordGrowthRefused_) return;  // the parse is being aborted; see wordGrowthRefused()

  word = utf8NfcNorm(std::move(word));

  // Chinese and Japanese are written without spaces: a word with CJK characters becomes one token
  // per break the CJK rules allow, so a line can end between its characters. Latin words get no
  // offsets and stay whole. Our implementation of crosspoint-reader PR #2288 ("feat: Support for
  // Korean line breaks and glyph spacing", Justin Mitchell, 22f35750).
  //
  // Every CJK mark whose width the punctuation table can change is a token of its own as well, glued
  // where kinsoku forbids a break (字。 stays together, but 。 gets its own x position): a token is
  // drawn at its font's advances, so only a mark that is a token can be drawn narrower (spec 5.2).
  const std::vector<size_t> cjkBreaks = CjkLineBreak::breakByteOffsets(word);
  // Only in a word with CJK text: a Latin word keeps its curly quotes inside it.
  std::vector<size_t> markEdges;  // byte offsets around each adjustable mark, glued unless a break is allowed
  bool hasCjk = false;
  for (const auto* q = reinterpret_cast<const unsigned char*>(word.c_str()); *q && !hasCjk;) {
    hasCjk = utf8IsCjkBreakable(utf8NextCodepoint(&q));
  }
  if (hasCjk) {
    const auto* const begin = reinterpret_cast<const unsigned char*>(word.c_str());
    const auto* p = begin;
    while (*p) {
      const size_t at = static_cast<size_t>(p - begin);
      const uint32_t cp = utf8NextCodepoint(&p);
      if (cp == 0) break;
      const CjkPunctuation::Class c = CjkPunctuation::classOf(cp);
      if (c == CjkPunctuation::Class::None || c == CjkPunctuation::Class::IdeographicSpace ||
          c == CjkPunctuation::Class::AmbiguousQuote) {
        continue;
      }
      const size_t after = static_cast<size_t>(p - begin);
      if (at > 0) markEdges.push_back(at);
      if (after < word.size()) markEdges.push_back(after);
    }
  }
  // The token boundaries: the CJK breaks, then each mark edge that is not already one (glued).
  struct Cut {
    size_t at;
    bool breakable;
  };
  std::vector<Cut> cuts;
  cuts.reserve(cjkBreaks.size() + markEdges.size());
  for (const size_t at : cjkBreaks) cuts.push_back({at, true});
  for (const size_t at : markEdges) {
    if (std::find(cjkBreaks.begin(), cjkBreaks.end(), at) == cjkBreaks.end()) cuts.push_back({at, false});
  }
  std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.at < b.at; });
  cuts.erase(std::unique(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.at == b.at; }),
             cuts.end());
  if (!reserveWords(cuts.size() + 1)) return;

  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  const uint8_t size = std::min(std::max(sizePct, MIN_WORD_SIZE_PCT), MAX_WORD_SIZE_PCT);

  // Glued to the previous token in the source (no whitespace between them): where the CJK rules
  // allow a break there, the two meet at a breakable zero-width gap. Where the source had
  // whitespace, the space is content and stays. From crosspoint-reader PR #2768 ("fix: restore
  // spaces between CJK words", Uri Tauber, 9163149a). A glued Latin "foo—" + "bar" stays glued: the gap needs
  // a CJK character on one side, since the dash rule of upstream PR #3883 is not ported.
  TokenBoundary::Boundary boundary{attachToPrevious, false};
  // A glued " " is the parser's no-break space (U+00A0 / U+202F): no break on either side of it.
  if (attachToPrevious && !words.empty() && words.back() != " " && word != " ") {
    const uint32_t left = lastCodepoint(words.back());
    const uint32_t right = firstCodepoint(word);
    if ((utf8IsCjkBreakable(left) || utf8IsCjkBreakable(right)) && CjkLineBreak::isGapBetween(left, right)) {
      boundary = TokenBoundary::Boundary{false, true};
    }
  }

  if (cuts.empty()) {
    pushToken(std::move(word), combinedStyle, boundary, size, visibleOffset, fontSlot);
    return;
  }
  size_t start = 0;
  uint32_t offset = visibleOffset;
  for (size_t i = 0; i <= cuts.size(); ++i) {
    const size_t end = i < cuts.size() ? cuts[i].at : word.size();
    pushToken(word.substr(start, end - start), combinedStyle, boundary, size, offset, fontSlot);
    // The next token starts where this one's visible bytes end (KOSync positions).
    offset += static_cast<uint32_t>(VisibleText::visibleBytes(word.data() + start, static_cast<int>(end - start)));
    if (i < cuts.size()) {
      // A CJK break is a breakable zero-width gap; a mark's other edges are glued (no space, no break).
      boundary = cuts[i].breakable ? TokenBoundary::Boundary{false, true} : TokenBoundary::Boundary{true, false};
    }
    start = end;
  }
}

bool ParsedText::foldUniformWordSizes() {
  if (words.empty() || wordSizes.empty()) return false;
  const uint8_t first = wordSizes[0].pct;
  for (const TextBlock::WordSize& sz : wordSizes) {
    if (sz.pct != first) return false;
  }
  if (first == DEFAULT_WORD_SIZE_PCT) return false;  // all-100% — nothing to fold

  blockStyle.fontSizeMultiplier *= first / 100.0f;
  for (TextBlock::WordSize& sz : wordSizes) sz.pct = DEFAULT_WORD_SIZE_PCT;
  return true;
}

// Consumes data to minimize memory usage
int ParsedText::widthForLine(const int lineIndex, const int lineHeight, const int16_t blockStartY,
                             const int pageWidth) const {
  if (lineHeight == 0 || blockStyle.floatZoneCount == 0) return pageWidth;
  const int lineTop = blockStartY + lineIndex * lineHeight;
  int indent = 0;
  for (int i = 0; i < blockStyle.floatZoneCount; ++i) {
    const auto& z = blockStyle.floatZones[i];
    if (lineTop < z.bottom && lineTop + lineHeight > z.top) {
      indent += z.width;
    }
  }
  return std::max(1, pageWidth - indent);
}

int ParsedText::spaceBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  return renderer.getSpaceAdvance(wordFontId(j - 1, fontId), lastCodepoint(words[j - 1]), firstCodepoint(words[j]),
                                  wordStyles[j - 1]);
}

int ParsedText::kernBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  const int left = wordFontId(j - 1, fontId);
  if (left != wordFontId(j, fontId)) return 0;
  return renderer.getKerning(left, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
}

int ParsedText::naturalGapBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  const TokenBoundary::Boundary boundary = wordBoundaries[j];
  if (boundary.continues) return kernBefore(renderer, fontId, j);
  if (boundary.noSpaceBefore) return 0;
  return spaceBefore(renderer, fontId, j);
}

void ParsedText::layoutAndExtractLines(
    const GfxRenderer& renderer, const int bodyFontId, const uint16_t viewportWidth,
    const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
    const bool includeLastLine, const int16_t blockStartY, const int lineHeight, const bool preserveSource) {
  if (words.empty()) {
    return;
  }

  // preserveSource: take a snapshot now and put it back on the way out, so the caller sees the
  // text exactly as it handed it over. Suppressing the trailing erase alone is not enough --
  // computeLineBreaks force-splits any word too wide for the line (inserting a hyphen), and
  // applyParagraphIndent/applyBionicReadingTransform rewrite words too. A cell measured against a
  // 47px column and then re-laid out as a full-width paragraph would otherwise keep the narrow
  // column's breaks: "Wordy0001" coming back as "Word-" + "y0001".
  //
  // The copy is bounded by the caller: only the table grid path sets this, and a cell is size-
  // bounded before it gets here. It lives for one cell's layout and is released on return.
  std::vector<std::string> savedWords;
  std::vector<EpdFontFamily::Style> savedStyles;
  std::vector<TextBlock::WordSize> savedSizes;
  std::vector<uint32_t> savedOffsets;
  std::vector<TokenBoundary::Boundary> savedBoundaries;
  bool savedIsContinuation = false;
  size_t savedBionicWatermark = 0;
  if (preserveSource) {
    savedWords = words;
    savedStyles = wordStyles;
    savedSizes = wordSizes;
    savedOffsets = wordVisibleOffsets;
    savedBoundaries = wordBoundaries;
    savedIsContinuation = isContinuation_;
    savedBionicWatermark = bionicTransformedUpTo_;
  }

  // Heading blocks lay out (and below render) with a taller real font instead of scaling the
  // body font. Resolve the effective fontId once; all measurement helpers below take it as
  // their fontId argument, so widths/kerning/indent are computed in the heading font and stay
  // consistent with TextBlock::render(). For the scale-fallback path headingFontId==0 and this
  // is just the body font (fontSizeMultiplier still applies inside the helpers).
  const int fontId = blockStyle.headingFontId != 0 ? blockStyle.headingFontId : bodyFontId;

  // Apply fixed transforms before any per-line layout work.
  // Paragraph indent only applies to the first layout pass; skip on continuations.
  if (!isContinuation_) {
    applyParagraphIndent(renderer, fontId);
  }
  // Bionic transform is incremental: applyBionicReadingTransform() is a no-op
  // for already-transformed words (bionicTransformedUpTo_ == words.size()) and
  // only processes raw words appended since the last flush, so it is always safe
  // to call regardless of isContinuation_.
  if (bionicReadingEnabled) {
    applyBionicReadingTransform();
  }

  // Ensure font glyph metrics are loaded before measuring word widths.
  // For built-in fonts this is a no-op (the map lookup finds nothing and returns
  // immediately). For SD card fonts it reads glyph metadata (advanceX, no bitmaps)
  // for all codepoints in this paragraph so calculateWordWidths() needs no SD I/O.
  // Each font gets its own words: the block's font, then each font slot in use (a TTF
  // family renders the glyphs it is handed, so not the other families' text).
  size_t totalSize = 1;  // reserve room for a possible hyphen fallback
  for (size_t i = 0; i < words.size(); i++) {
    if (i > 0 && TokenBoundary::hasSpace(wordBoundaries[i])) totalSize += 1;
    totalSize += words[i].size();
  }
  for (int slot = -1; slot < TextBlock::kMaxWordFonts; slot++) {
    const int32_t font = slot < 0 ? 0 : fontSlots_[slot].fontId;  // 0: the block's
    if (slot >= 0 && (font == 0 || (fontSlotsUsed_ & (1u << slot)) == 0)) continue;
    std::string& allText = allText_;
    allText.clear();
    allText.reserve(totalSize);
    for (size_t i = 0; i < words.size(); i++) {
      if (fontSlots_[wordSizes[i].fontSlot].fontId != font) continue;
      if (!allText.empty() && TokenBoundary::hasSpace(wordBoundaries[i])) allText += ' ';
      allText += words[i];
    }
    allText += '-';
    renderer.ensureFontReady(font != 0 ? font : fontId, allText.c_str());
  }

  const int pageWidth = viewportWidth;

  // Compute firstLineIndent once here so all layout helpers use the same value.
  // On a continuation flush the remaining words are mid-paragraph, so no indent.
  // firstLineExtraIndent adds extra indent on the first line (on top of CSS text-indent).
  // A negative text-indent is the hanging-indent pattern (margin-left:3em; text-indent:-1em),
  // where the inset is what the first line hangs out of. It may pull left by at most that inset:
  // the block's own left edge is where the CSS containing block starts, and anything past it
  // renders off the panel and silently loses its first glyphs (issue #198).
  const int cssTextIndent =
      !isContinuation_ && blockStyle.textIndentDefined &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? std::min(std::max<int>(static_cast<int>(blockStyle.textIndent), -static_cast<int>(blockStyle.leftInset())),
                     pageWidth - 1)
          : 0;
  int firstLineIndent = cssTextIndent + (isContinuation_ ? 0 : static_cast<int>(blockStyle.firstLineExtraIndent));

  std::vector<uint16_t>& wordWidths = wordWidths_;
  calculateWordWidths(renderer, fontId, wordWidths);

  // A span indent gives way before the line does. Poems draw their shape with margin-left on each
  // line's span (Alice's mouse tale steps out to 10em and back); inside a block that is already
  // inset, the deepest lines no longer fit, and wrapping put the rest of the line back at the
  // column's left edge -- the shape broke wherever that happened. A block whose words fit the
  // column on one line now keeps to one line, its indent shortened by what it lacks. Only the
  // span-indent channel: a paragraph's own text-indent is typography, not shape, and wraps as
  // before.
  if (blockStyle.textIndentYields && cssTextIndent > 0 && includeLastLine && !words.empty()) {
    int natural = wordWidths[0];
    for (size_t j = 1; j < words.size(); ++j) {
      natural += wordWidths[j] + naturalGapBefore(renderer, fontId, j);
    }
    const int available = widthForLine(0, lineHeight, blockStartY, pageWidth);
    if (natural <= available && natural + firstLineIndent > available) {
      firstLineIndent = std::max(0, available - natural);
    }
  }

  std::vector<size_t>& lineBreakIndices = lineBreakIndices_;
  std::vector<bool>& lineEndsWithHyphenatedWord = lineEndsWithHyphenatedWord_;
  std::vector<int>& splitPrefixWordIndexes = splitPrefixWordIndexes_;
  std::vector<bool>& splitInsertedHyphen = splitInsertedHyphen_;
  lineEndsWithHyphenatedWord.clear();
  splitPrefixWordIndexes.clear();
  splitInsertedHyphen.clear();
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordBoundaries, lineEndsWithHyphenatedWord,
                                splitPrefixWordIndexes, splitInsertedHyphen, firstLineIndent, blockStartY, lineHeight,
                                lineBreakIndices);
  } else {
    computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordBoundaries, firstLineIndent, blockStartY, lineHeight,
                      lineBreakIndices);
    lineEndsWithHyphenatedWord.assign(lineBreakIndices.size(), false);
    splitPrefixWordIndexes.assign(lineBreakIndices.size(), -1);
    splitInsertedHyphen.assign(lineBreakIndices.size(), false);
  }
  size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    const bool lineEndedWithHyphenation = i < lineEndsWithHyphenatedWord.size() ? lineEndsWithHyphenatedWord[i] : false;
    // A retried line below keeps this place: it is the same line, laid out again.
    linePlace_.paragraphStart = !isContinuation_ && i == 0;
    linePlace_.linesLeft =
        includeLastLine ? static_cast<uint16_t>(std::min<size_t>(lineCount - i, std::numeric_limits<uint16_t>::max()))
                        : 0;
    const auto result = extractLine(i, pageWidth, wordWidths, wordBoundaries, lineBreakIndices, processLine, renderer,
                                    fontId, lineEndedWithHyphenation, false, firstLineIndent, blockStartY, lineHeight);

    if (result == LineProcessResult::RetryWithoutHyphenation && lineEndedWithHyphenation) {
      const size_t lineStart = i > 0 ? lineBreakIndices[i - 1] : 0;
      const size_t lineEnd = i < lineBreakIndices.size() ? lineBreakIndices[i] : lineStart;
      const std::string firstAttemptPreview = buildLinePreview(words, wordBoundaries, lineStart, lineEnd);
      LOG_TRC("PTX", "Line %u requested rerender without hyphenation, first attempt: %s", static_cast<unsigned>(i),
              firstAttemptPreview.c_str());
      // Undo precomputed splits from this line onward so the retry starts from
      // clean, unsplit tokens and cannot inherit future hyphenation artifacts.
      std::set<int, std::greater<int>> splitIndexesToUndo;
      for (size_t lineIdx = i; lineIdx < splitPrefixWordIndexes.size(); ++lineIdx) {
        const int splitIndex = splitPrefixWordIndexes[lineIdx];
        if (splitIndex >= 0) {
          splitIndexesToUndo.insert(splitIndex);
        }
      }
      for (const int splitIndex : splitIndexesToUndo) {
        if (splitIndex < 0 || static_cast<size_t>(splitIndex + 1) >= words.size()) {
          continue;
        }
        bool removeInsertedHyphen = false;
        for (size_t lineIdx = i; lineIdx < splitPrefixWordIndexes.size(); ++lineIdx) {
          if (splitPrefixWordIndexes[lineIdx] == splitIndex && lineIdx < splitInsertedHyphen.size()) {
            removeInsertedHyphen = splitInsertedHyphen[lineIdx];
            break;
          }
        }

        std::string merged = words[splitIndex];
        if (removeInsertedHyphen && !merged.empty() && merged.back() == '-') {
          merged.pop_back();
        }
        merged += words[splitIndex + 1];
        words[splitIndex] = std::move(merged);
        words.erase(words.begin() + splitIndex + 1);
        wordStyles.erase(wordStyles.begin() + splitIndex + 1);
        wordBoundaries.erase(wordBoundaries.begin() + splitIndex + 1);
        wordSizes.erase(wordSizes.begin() + splitIndex + 1);
        wordVisibleOffsets.erase(wordVisibleOffsets.begin() + splitIndex + 1);
      }

      // Recompute widths after restoring unsplit words.
      calculateWordWidths(renderer, fontId, wordWidths);

      // Keep previous lines fixed; recompute only this specific line without hyphenation.
      // Suppression is intentionally line-local.
      const size_t retryBreak = computeSingleLineBreakNoHyphen(renderer, fontId, pageWidth, wordWidths, wordBoundaries,
                                                               lineStart, firstLineIndent, blockStartY, lineHeight);

      lineBreakIndices.resize(i + 1);
      lineEndsWithHyphenatedWord.resize(i + 1);
      splitPrefixWordIndexes.resize(i + 1);
      splitInsertedHyphen.resize(i + 1);

      lineBreakIndices[i] = retryBreak;
      lineEndsWithHyphenatedWord[i] = false;
      splitPrefixWordIndexes[i] = -1;
      splitInsertedHyphen[i] = false;
      lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

      if (i < lineBreakIndices.size()) {
        const size_t retryLineStart = i > 0 ? lineBreakIndices[i - 1] : 0;
        const size_t retryLineEnd = i < lineBreakIndices.size() ? lineBreakIndices[i] : retryLineStart;
        const std::string retryPreview = buildLinePreview(words, wordBoundaries, retryLineStart, retryLineEnd);
        LOG_TRC("PTX", "Rerendering line %u with hyphenation suppressed, retry attempt: %s", static_cast<unsigned>(i),
                retryPreview.c_str());
        extractLine(i, pageWidth, wordWidths, wordBoundaries, lineBreakIndices, processLine, renderer, fontId, false,
                    true, firstLineIndent, blockStartY, lineHeight);

        // Resume regular hyphenation from the first word after the retried line.
        const size_t resumeIndex = lineBreakIndices[i];
        std::vector<bool>& suffixLineEndsWithHyphenatedWord = suffixLineEndsWithHyphenatedWord_;
        std::vector<int>& suffixSplitPrefixWordIndexes = suffixSplitPrefixWordIndexes_;
        std::vector<bool>& suffixSplitInsertedHyphen = suffixSplitInsertedHyphen_;
        suffixLineEndsWithHyphenatedWord.clear();
        suffixSplitPrefixWordIndexes.clear();
        suffixSplitInsertedHyphen.clear();
        const std::vector<size_t>& hyphenatedSuffixBreaks = suffixBreaks_;
        computeHyphenatedLineBreaksFromIndex(renderer, fontId, pageWidth, wordWidths, wordBoundaries, resumeIndex,
                                             suffixLineEndsWithHyphenatedWord, suffixSplitPrefixWordIndexes,
                                             suffixSplitInsertedHyphen, blockStartY, lineHeight,
                                             static_cast<int>(i) + 1, suffixBreaks_);

        lineBreakIndices.insert(lineBreakIndices.end(), hyphenatedSuffixBreaks.begin(), hyphenatedSuffixBreaks.end());
        lineEndsWithHyphenatedWord.insert(lineEndsWithHyphenatedWord.end(), suffixLineEndsWithHyphenatedWord.begin(),
                                          suffixLineEndsWithHyphenatedWord.end());
        splitPrefixWordIndexes.insert(splitPrefixWordIndexes.end(), suffixSplitPrefixWordIndexes.begin(),
                                      suffixSplitPrefixWordIndexes.end());
        splitInsertedHyphen.insert(splitInsertedHyphen.end(), suffixSplitInsertedHyphen.begin(),
                                   suffixSplitInsertedHyphen.end());

        lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;
        LOG_TRC("PTX", "Resumed regular hyphenation after rerendered line %u", static_cast<unsigned>(i));
      }
    }
  }

  // Remove consumed words so size() reflects only remaining words. The capacity is kept on
  // purpose: the parser reuses this object for the next paragraph (see reset()), and a block is
  // split at 96 words (ChapterHtmlSlimParser::flushPartWordBuffer), so the vectors settle at
  // 128 entries and never grow again. Shrinking here used to hand the next paragraph an empty
  // vector to regrow through 16/32/64/128 -- the churn reset() exists to remove.
  //
  // preserveSource skips this: a caller that may have to lay the same text out a second time (the
  // table grid path, which can only discover a cell does not fit by laying it out) needs the words
  // to still be there. It pays for that in residency, so it is opt-in.
  if (lineCount > 0 && !preserveSource) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordBoundaries.erase(wordBoundaries.begin(), wordBoundaries.begin() + consumed);
    wordSizes.erase(wordSizes.begin(), wordSizes.begin() + consumed);
    wordVisibleOffsets.erase(wordVisibleOffsets.begin(), wordVisibleOffsets.begin() + consumed);
    // All remaining words were already transformed before the flush; reset the
    // watermark so that words appended by addWord() are processed next time.
    bionicTransformedUpTo_ = words.size();
  }
  isContinuation_ = !includeLastLine;

  if (preserveSource) {
    words = std::move(savedWords);
    wordStyles = std::move(savedStyles);
    wordSizes = std::move(savedSizes);
    wordVisibleOffsets = std::move(savedOffsets);
    wordBoundaries = std::move(savedBoundaries);
    isContinuation_ = savedIsContinuation;
    bionicTransformedUpTo_ = savedBionicWatermark;
  }
}

void ParsedText::releaseLayoutScratch() {
  // Pure scratch: recomputed by every layout, so safe to drop at any point between feeds.
  std::vector<uint16_t>().swap(wordWidths_);
  std::vector<size_t>().swap(lineBreakIndices_);
  std::vector<bool>().swap(lineEndsWithHyphenatedWord_);
  std::vector<int>().swap(splitPrefixWordIndexes_);
  std::vector<bool>().swap(splitInsertedHyphen_);
  std::vector<size_t>().swap(suffixBreaks_);
  std::vector<bool>().swap(suffixLineEndsWithHyphenatedWord_);
  std::vector<int>().swap(suffixSplitPrefixWordIndexes_);
  std::vector<bool>().swap(suffixSplitInsertedHyphen_);
  std::vector<int>().swap(interWordGaps_);
  std::vector<int>().swap(lineIndexForWord_);
  std::vector<int>().swap(dp_);
  std::vector<size_t>().swap(ans_);
  std::vector<int16_t>().swap(lineXPosScratch_);
  std::string().swap(allText_);
  // The word vectors carry the paragraph in progress: an empty block gives them back whole; a
  // block mid-paragraph keeps them AS THEY ARE. This used to shrink_to_fit them, and a slice
  // yields inside a paragraph almost always (the 1 KB feed chunk ends mid-<p>), so every yield
  // reallocated the vectors to their exact size and the next words regrew them by doubling
  // from there -- 40 -> 80 -> 160 entries, a 3,840 B request above the 128-entry steady state,
  // and a free/alloc pair per slice that left the heap at 3,956 B contiguous with 14 KB free
  // (device run 10). The capacity kept here is at most 128 entries, ~3.6 KB, reached once.
  if (words.empty()) {
    std::vector<std::string>().swap(words);
    std::vector<EpdFontFamily::Style>().swap(wordStyles);
    std::vector<TokenBoundary::Boundary>().swap(wordBoundaries);
    std::vector<TextBlock::WordSize>().swap(wordSizes);
    std::vector<uint32_t>().swap(wordVisibleOffsets);
  }
}

void ParsedText::reset(const BlockStyle& newBlockStyle) {
  // clear() keeps each vector's capacity; the strings inside `words` are destroyed here.
  words.clear();
  wordStyles.clear();
  wordBoundaries.clear();
  wordSizes.clear();
  wordVisibleOffsets.clear();
  for (TextBlock::WordFont& slot : fontSlots_) slot = {};
  fontSlotsUsed_ = 0;
  blockStyle = newBlockStyle;
  isContinuation_ = false;
  bionicTransformedUpTo_ = 0;
}

void ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId, std::vector<uint16_t>& out) {
  out.clear();
  out.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    out.push_back(measureWordWidth(renderer, wordFontId(i, fontId), words[i], wordStyles[i], false, wordScale(i)));
  }
  applyPunctuationWidths(renderer, fontId, out);
}

void ParsedText::applyPunctuationWidths(const GfxRenderer& renderer, const int fontId, std::vector<uint16_t>& widths) {
  using CjkPunctuation::Class;
  const size_t count = words.size();
  lineStartTrim_.assign(count, 0);
  lineEndTrim_.assign(count, 0);

  // A mark is CJK punctuation only where its font draws it full-width, at least 4/5 of the em of 一 in the same
  // face and size: a curly quote in a Latin font is narrow. Every token is judged on its natural width before any
  // is narrowed, as a narrowed neighbour (” at 6/8 before “) would no longer pass.
  int emFont = -1;
  EpdFontFamily::Style emStyle = EpdFontFamily::REGULAR;
  float emScale = 0.0f;
  int em = 0;
  const auto fullWidth = [&](const size_t i) {
    const EpdFontFamily::Style style = static_cast<EpdFontFamily::Style>(wordStyles[i] & EpdFontFamily::BOLD_ITALIC);
    if (wordFontId(i, fontId) != emFont || style != emStyle || wordScale(i) != emScale) {
      emFont = wordFontId(i, fontId);
      emStyle = style;
      emScale = wordScale(i);
      em = measureWordWidth(renderer, emFont, "\xE4\xB8\x80", emStyle, false, emScale);  // 一
    }
    return em > 0 && widths[i] * 5 >= em * 4;
  };
  const auto singleCodepoint = [&](const size_t i) {
    const auto* p = reinterpret_cast<const unsigned char*>(words[i].c_str());
    const uint32_t cp = utf8NextCodepoint(&p);
    return *p == 0 ? cp : 0;
  };

  // The class of each token that is one full-width mark (addWord makes every adjustable mark a token).
  std::vector<uint8_t>& marks = markClass_;
  marks.assign(count, static_cast<uint8_t>(Class::None));
  for (size_t i = 0; i < count; ++i) {
    const uint32_t cp = singleCodepoint(i);
    const Class c = cp != 0 ? CjkPunctuation::classOf(cp) : Class::None;
    if (c != Class::None && fullWidth(i)) marks[i] = static_cast<uint8_t>(c);
  }
  // The class of token j's character next to the mark: a mark token's own, else the edge codepoint's (a curly quote
  // inside a word is Latin).
  const auto neighbourClass = [&](const size_t j, const uint32_t edge) {
    if (singleCodepoint(j) != 0) return static_cast<Class>(marks[j]);
    return CjkPunctuation::isCurlyQuote(edge) ? Class::None : CjkPunctuation::classOf(edge);
  };

  const auto table = typography_.punctuation;
  for (size_t i = 0; i < count; ++i) {
    const auto mark = static_cast<Class>(marks[i]);
    if (mark == Class::None || mark == Class::IdeographicSpace || mark == Class::AmbiguousQuote) continue;
    // An opening mark looks at the character before it, every other mark at the one after it; across a space there
    // is no neighbour.
    Class neighbour = Class::None;
    if (mark == Class::Opening) {
      if (i > 0 && !TokenBoundary::hasSpace(wordBoundaries[i])) {
        neighbour = neighbourClass(i - 1, lastCodepoint(words[i - 1]));
      }
    } else if (i + 1 < count && !TokenBoundary::hasSpace(wordBoundaries[i + 1])) {
      neighbour = neighbourClass(i + 1, firstCodepoint(words[i + 1]));
    }
    const int full = widths[i];
    const int mid = (full * CjkPunctuation::widthOf(table, mark, neighbour, false).forced + 4) / 8;
    const int edge = (full * CjkPunctuation::widthOf(table, mark, Class::None, true).forced + 4) / 8;
    widths[i] = static_cast<uint16_t>(mid);
    (mark == Class::Opening ? lineStartTrim_ : lineEndTrim_)[i] = static_cast<int16_t>(edge - mid);
  }
}

void ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                   std::vector<uint16_t>& wordWidths,
                                   const std::vector<TokenBoundary::Boundary>& boundaries, const int firstLineIndent,
                                   const int16_t blockStartY, const int lineHeight,
                                   std::vector<size_t>& lineBreakIndices) {
  lineBreakIndices.clear();
  if (words.empty()) {
    return;
  }

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = widthForLine(0, lineHeight, blockStartY, pageWidth) - (i == 0 ? firstLineIndent : 0);
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // Pre-compute inter-word gaps once so the O(n²) DP inner loop avoids repeated
  // codepoint scanning and renderer calls for every (i,j) pair.
  // interWordGaps[j] = the spacing between words[j-1] and words[j] (0 for j==0).
  // The four tables below are members reused across paragraphs (see the header): as locals
  // they cost four allocations per paragraph, the last per-block churn on a plain layout.
  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(totalWordCount, 0);
  for (size_t j = 1; j < totalWordCount; ++j) {
    interWordGaps[j] = naturalGapBefore(renderer, fontId, j);
  }

  // Greedy pre-pass: map each word-start index to its line number so the DP can
  // call widthForLine(lineIdx, ...) per-word-start in O(1).
  std::vector<int>& lineIndexForWord = lineIndexForWord_;
  lineIndexForWord.assign(totalWordCount, 0);
  if (lineHeight > 0 && blockStyle.floatZoneCount > 0) {
    int lineIdx = 0;
    size_t cur = 0;
    while (cur < totalWordCount) {
      const int lineW =
          widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (lineIdx == 0 ? firstLineIndent : 0);
      int w = 0;
      size_t start = cur;
      while (cur < totalWordCount) {
        const int gap = (cur > start) ? interWordGaps[cur] : 0;
        if (w + gap + wordWidths[cur] > lineW && cur > start) break;
        lineIndexForWord[cur] = lineIdx;
        w += gap + wordWidths[cur];
        ++cur;
      }
      // Guard: if no progress (single oversized word), advance anyway
      if (cur == start) {
        lineIndexForWord[cur] = lineIdx;
        ++cur;
      }
      ++lineIdx;
    }
  }

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int>& dp = dp_;
  dp.assign(totalWordCount, 0);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t>& ans = ans_;
  ans.assign(totalWordCount, 0);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    const int lineIdx = lineIndexForWord[i];
    const int effectivePageWidth =
        widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (lineIdx == 0 ? firstLineIndent : 0);

    for (size_t j = i; j < totalWordCount; ++j) {
      const int gap = (j > static_cast<size_t>(i)) ? interWordGaps[j] : 0;
      currlen += wordWidths[j] + gap;
      // The line as drawn: its first and last token at their line-edge widths (CJK punctuation, spec 5.2). It only
      // grows with j, as a token is always wider than what a line edge takes off it.
      const int lineLen = withEdgeTrims(currlen, i, j);

      if (lineLen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && !TokenBoundary::allowsBreak(boundaries[j + 1])) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line — no penalty regardless of looseness
      } else {
        const int remainingSpace = effectivePageWidth - lineLen;
        // Knuth-Plass style demerits:
        //   badness  = (gap/lineWidth)³ × 10000, clamped to [0, 10000]
        //   demerits = (1 + badness)²
        // Cubic badness strongly penalises very loose lines while being
        // lenient on moderately loose ones, producing visually balanced paragraphs.
        const long long b_num = static_cast<long long>(remainingSpace) * remainingSpace * remainingSpace;
        const long long b_den = static_cast<long long>(effectivePageWidth) * effectivePageWidth * effectivePageWidth;
        const int badness = (b_den > 0) ? static_cast<int>(std::min(b_num * 10000LL / b_den, 10000LL)) : 10000;
        const long long demerits = static_cast<long long>(1 + badness) * (1 + badness);
        const long long cost_ll = demerits + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return;
}

size_t ParsedText::computeSingleLineBreakNoHyphen(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const std::vector<uint16_t>& wordWidths,
                                                  const std::vector<TokenBoundary::Boundary>& boundaries,
                                                  const size_t lineStartIndex, const int firstLineIndent,
                                                  const int16_t blockStartY, const int lineHeight) const {
  // One-line non-hyphenating breaker used by the page-boundary retry path.
  if (lineStartIndex >= wordWidths.size()) {
    return lineStartIndex;
  }

  // lineStartIndex == 0 means this is the first line of the block.
  const int effectivePageWidth = widthForLine(lineStartIndex == 0 ? 0 : 1, lineHeight, blockStartY, pageWidth) -
                                 (lineStartIndex == 0 ? firstLineIndent : 0);

  size_t currentIndex = lineStartIndex;
  int lineWidth = 0;

  while (currentIndex < wordWidths.size()) {
    const bool isFirstWord = currentIndex == lineStartIndex;
    const int spacing = isFirstWord ? 0 : naturalGapBefore(renderer, fontId, currentIndex);

    const int candidateWidth = spacing + wordWidths[currentIndex];
    if (withEdgeTrims(lineWidth + candidateWidth, lineStartIndex, currentIndex) <= effectivePageWidth) {
      lineWidth += candidateWidth;
      ++currentIndex;
      continue;
    }

    if (currentIndex == lineStartIndex) {
      ++currentIndex;
    }
    break;
  }

  while (currentIndex > lineStartIndex + 1 && currentIndex < wordWidths.size() &&
         !TokenBoundary::allowsBreak(boundaries[currentIndex])) {
    --currentIndex;
  }

  return currentIndex;
}

void ParsedText::applyParagraphIndent(const GfxRenderer& renderer, const int fontId) {
  if (words.empty()) {
    return;
  }

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) — handled by extractLine() via firstLineIndent.
  } else if (blockStyle.fromBrElement) {
    // A block created by an inline <br> is the next line of the SAME paragraph, so it gets no
    // first-line indent. (A <br> standing between two paragraphs leaves its block empty; that
    // block is merged into the following one, which brings its own indent.)
  } else if (!extraParagraphSpacing && blockStyle.floatZoneCount == 0 &&
             (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)) {
    // No CSS text-indent defined — apply a font-size-relative pixel indent so paragraph
    // boundaries are visually clear. Using getFontAscenderSize() gives one em in pixels,
    // matching the typographic convention for a paragraph indent. This avoids injecting
    // U+2003 (em-space) as a character, which caused glyph-miss overhead on every page
    // when the active font doesn't contain that codepoint.
    // Skip when a float zone is active: the image already provides visual separation.
    const int oneEm = static_cast<int>(renderer.getFontAscenderSize(fontId) * blockStyle.fontSizeMultiplier + 0.5f);
    blockStyle.textIndent = oneEm;
    blockStyle.textIndentDefined = true;
  }
}

void ParsedText::applyBionicReadingTransform() {
  // Only transform words that haven't been processed yet.  On a fresh block
  // bionicTransformedUpTo_ == 0 so all words are processed.  After an
  // intermediate flush, only the new raw words appended since the last flush
  // (indices bionicTransformedUpTo_..words.size()-1) need transformation.
  if (words.empty() || bionicTransformedUpTo_ >= words.size()) {
    return;
  }

  const size_t suffixStart = bionicTransformedUpTo_;
  std::vector<std::string> transformedSuffix;
  std::vector<EpdFontFamily::Style> transformedSuffixStyles;
  std::vector<TokenBoundary::Boundary> transformedSuffixBoundaries;
  std::vector<TextBlock::WordSize> transformedSuffixSizes;
  // Each original word became one or two transformed words; both halves keep the word's offset.
  std::vector<uint32_t> transformedSuffixOffsets;
  transformedSuffix.reserve((words.size() - suffixStart) * 2);
  transformedSuffixStyles.reserve(transformedSuffix.capacity());
  transformedSuffixBoundaries.reserve(transformedSuffix.capacity());
  transformedSuffixSizes.reserve(transformedSuffix.capacity());
  transformedSuffixOffsets.reserve(transformedSuffix.capacity());

  for (size_t i = suffixStart; i < words.size(); ++i) {
    std::string source = std::move(words[i]);
    const auto originalStyle = wordStyles[i];
    const TokenBoundary::Boundary originalBoundary = wordBoundaries[i];
    const TextBlock::WordSize originalSize = wordSizes[i];
    const uint32_t originalOffset = wordVisibleOffsets[i];

    const auto spans = tokenizeBionicWord(source);
    if (spans.empty()) {
      continue;
    }

    // The first span keeps the word's own boundary; every span after it is glued to the one before.
    constexpr TokenBoundary::Boundary kGlued{true, false};
    TokenBoundary::Boundary boundary = originalBoundary;
    for (size_t spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
      const TokenSpan span = spans[spanIndex];
      const size_t spanLength = span.end - span.start;
      std::string token = source.substr(span.start, spanLength);

      if (span.isWord) {
        const unsigned char* ptr = reinterpret_cast<const unsigned char*>(token.c_str());
        int codepointCount = 0;
        while (utf8NextCodepoint(&ptr)) {
          codepointCount++;
        }

        if (codepointCount >= kBionicReadingMinCodepoints) {
          const int boldPrefixCount = computeBionicBoldPrefixCount(codepointCount);
          ptr = reinterpret_cast<const unsigned char*>(token.c_str());
          const unsigned char* prefixEnd = ptr;
          for (int j = 0; j < boldPrefixCount && *prefixEnd; ++j) {
            utf8NextCodepoint(&prefixEnd);
          }
          const size_t prefixByteCount =
              static_cast<size_t>(prefixEnd - reinterpret_cast<const unsigned char*>(token.c_str()));
          if (prefixByteCount < token.size()) {
            std::string suffix(reinterpret_cast<const char*>(prefixEnd), token.size() - prefixByteCount);
            token.resize(prefixByteCount);
            const auto boldStyle = static_cast<EpdFontFamily::Style>(originalStyle | EpdFontFamily::BOLD);
            transformedSuffix.push_back(std::move(token));
            transformedSuffixStyles.push_back(boldStyle);
            transformedSuffixBoundaries.push_back(boundary);
            transformedSuffixSizes.push_back(originalSize);
            transformedSuffixOffsets.push_back(originalOffset);

            transformedSuffix.push_back(std::move(suffix));
            transformedSuffixStyles.push_back(originalStyle);
            transformedSuffixBoundaries.push_back(kGlued);
            transformedSuffixSizes.push_back(originalSize);
            transformedSuffixOffsets.push_back(originalOffset);
            boundary = kGlued;
            continue;
          }
        }
      }

      transformedSuffix.push_back(std::move(token));
      transformedSuffixStyles.push_back(originalStyle);
      transformedSuffixBoundaries.push_back(boundary);
      transformedSuffixSizes.push_back(originalSize);
      transformedSuffixOffsets.push_back(originalOffset);
      boundary = kGlued;
    }
  }

  // Replace the (now move-emptied) suffix with the transformed version.
  words.resize(suffixStart);
  wordStyles.resize(suffixStart);
  wordBoundaries.resize(suffixStart);
  wordSizes.resize(suffixStart);
  wordVisibleOffsets.resize(suffixStart);
  words.insert(words.end(), std::make_move_iterator(transformedSuffix.begin()),
               std::make_move_iterator(transformedSuffix.end()));
  wordStyles.insert(wordStyles.end(), transformedSuffixStyles.begin(), transformedSuffixStyles.end());
  wordBoundaries.insert(wordBoundaries.end(), transformedSuffixBoundaries.begin(), transformedSuffixBoundaries.end());
  wordSizes.insert(wordSizes.end(), transformedSuffixSizes.begin(), transformedSuffixSizes.end());
  wordVisibleOffsets.insert(wordVisibleOffsets.end(), transformedSuffixOffsets.begin(), transformedSuffixOffsets.end());
  bionicTransformedUpTo_ = words.size();
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
void ParsedText::computeHyphenatedLineBreaks(
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, std::vector<bool>& lineEndsWithHyphenatedWord,
    std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen, const int firstLineIndent,
    const int16_t blockStartY, const int lineHeight, std::vector<size_t>& lineBreakIndices) {
  lineBreakIndices.clear();
  // Pre-compute inter-word gaps to avoid repeated codepoint scanning and renderer
  // calls in the inner loop. When hyphenateWordAtIndex inserts a new word, we insert
  // a placeholder gap (0) at that position to keep the vector in sync; the remainder
  // is always the first word on the next line so its spacing is never used.
  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(wordWidths.size(), 0);
  for (size_t j = 1; j < wordWidths.size(); ++j) {
    interWordGaps[j] = naturalGapBefore(renderer, fontId, j);
  }

  lineEndsWithHyphenatedWord.clear();
  splitPrefixWordIndexes.clear();
  splitInsertedHyphen.clear();
  size_t currentIndex = 0;
  int lineIdx = 0;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    bool lineEndedWithHyphenation = false;
    int splitPrefixIndex = -1;
    bool splitNeedsInsertedHyphen = false;

    const int effectivePageWidth =
        widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (lineIdx == 0 ? firstLineIndent : 0);

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord ? 0 : interWordGaps[currentIndex];
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (withEdgeTrims(lineWidth + candidateWidth, lineStart, currentIndex) <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      bool insertedHyphen = false;
      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &insertedHyphen)) {
        // Keep interWordGaps in sync: insert placeholder for the new remainder word.
        // The remainder is always the first word on the next line so this slot is never read.
        interWordGaps.insert(interWordGaps.begin() + currentIndex + 1, 0);
        lineEndedWithHyphenation = true;
        splitPrefixIndex = static_cast<int>(currentIndex);
        splitNeedsInsertedHyphen = insertedHyphen;
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() &&
           !TokenBoundary::allowsBreak(boundaries[currentIndex])) {
      --currentIndex;
    }

    if (lineEndedWithHyphenation &&
        (splitPrefixIndex < static_cast<int>(lineStart) || splitPrefixIndex >= static_cast<int>(currentIndex))) {
      lineEndedWithHyphenation = false;
      splitPrefixIndex = -1;
      splitNeedsInsertedHyphen = false;
    }

    lineBreakIndices.push_back(currentIndex);
    lineEndsWithHyphenatedWord.push_back(lineEndedWithHyphenation);
    splitPrefixWordIndexes.push_back(splitPrefixIndex);
    splitInsertedHyphen.push_back(splitNeedsInsertedHyphen);
    ++lineIdx;
  }

  return;
}

void ParsedText::computeHyphenatedLineBreaksFromIndex(
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, const size_t startIndex,
    std::vector<bool>& lineEndsWithHyphenatedWord, std::vector<int>& splitPrefixWordIndexes,
    std::vector<bool>& splitInsertedHyphen, const int16_t blockStartY, const int lineHeight, const int startLineIdx,
    std::vector<size_t>& lineBreakIndices) {
  // Same greedy hyphenating breaker as the full pass, but scoped to a suffix.
  lineBreakIndices.clear();
  if (startIndex >= wordWidths.size()) {
    lineEndsWithHyphenatedWord.clear();
    splitPrefixWordIndexes.clear();
    splitInsertedHyphen.clear();
    return;
  }

  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(wordWidths.size(), 0);
  for (size_t j = 1; j < wordWidths.size(); ++j) {
    interWordGaps[j] = naturalGapBefore(renderer, fontId, j);
  }

  lineEndsWithHyphenatedWord.clear();
  splitPrefixWordIndexes.clear();
  splitInsertedHyphen.clear();

  size_t currentIndex = startIndex;
  int lineIdx = startLineIdx;
  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    bool lineEndedWithHyphenation = false;
    int splitPrefixIndex = -1;
    bool splitNeedsInsertedHyphen = false;
    const int effectivePageWidth = widthForLine(lineIdx, lineHeight, blockStartY, pageWidth);

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord ? 0 : interWordGaps[currentIndex];
      const int candidateWidth = spacing + wordWidths[currentIndex];

      if (withEdgeTrims(lineWidth + candidateWidth, lineStart, currentIndex) <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;

      bool insertedHyphen = false;
      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &insertedHyphen)) {
        interWordGaps.insert(interWordGaps.begin() + currentIndex + 1, 0);
        lineEndedWithHyphenation = true;
        splitPrefixIndex = static_cast<int>(currentIndex);
        splitNeedsInsertedHyphen = insertedHyphen;
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() &&
           !TokenBoundary::allowsBreak(boundaries[currentIndex])) {
      --currentIndex;
    }

    if (lineEndedWithHyphenation &&
        (splitPrefixIndex < static_cast<int>(lineStart) || splitPrefixIndex >= static_cast<int>(currentIndex))) {
      lineEndedWithHyphenation = false;
      splitPrefixIndex = -1;
      splitNeedsInsertedHyphen = false;
    }

    lineBreakIndices.push_back(currentIndex);
    lineEndsWithHyphenatedWord.push_back(lineEndedWithHyphenation);
    splitPrefixWordIndexes.push_back(splitPrefixIndex);
    splitInsertedHyphen.push_back(splitNeedsInsertedHyphen);
    ++lineIdx;
  }

  return;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks, bool* outInsertedHyphen) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];
  const float scale = wordScale(wordIndex);
  const int font = wordFontId(wordIndex, fontId);

  // Collect candidate breakpoints (byte offsets and hyphen requirements). A Hangul word may split
  // between syllables at a line end, without a hyphen; those breaks come first, so they win a width
  // tie. From crosspoint-reader PR #3700 ("fix: justify Korean text by word spaces only", Sung-jin
  // Brian Hong).
  std::vector<Hyphenator::BreakInfo> breakInfos = CjkLineBreak::hangulLineEndBreaks(word);
  const std::vector<Hyphenator::BreakInfo> hyphenatorBreaks = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  breakInfos.insert(breakInfos.end(), hyphenatorBreaks.begin(), hyphenatorBreaks.end());
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, font, word.substr(0, offset), style, needsHyphen, scale);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style, size and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  wordSizes.insert(wordSizes.begin() + wordIndex + 1, wordSizes[wordIndex]);
  // The remainder starts where the prefix's visible bytes end (the hyphen is not source text).
  wordVisibleOffsets.insert(
      wordVisibleOffsets.begin() + wordIndex + 1,
      wordVisibleOffsets[wordIndex] +
          static_cast<uint32_t>(
              VisibleText::visibleBytes(words[wordIndex].data(), static_cast<int>(words[wordIndex].size())) -
              (chosenNeedsHyphen ? 1 : 0)));

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordBoundaries[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordBoundaries.insert(wordBoundaries.begin() + wordIndex + 1, TokenBoundary::Boundary{});

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, font, remainder, style, false, scale);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  lineStartTrim_.insert(lineStartTrim_.begin() + wordIndex + 1, 0);
  lineEndTrim_.insert(lineEndTrim_.begin() + wordIndex + 1, 0);
  if (outInsertedHyphen) {
    *outInsertedHyphen = chosenNeedsHyphen;
  }
  return true;
}

ParsedText::LineProcessResult ParsedText::extractLine(
    const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, const std::vector<size_t>& lineBreakIndices,
    const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
    const GfxRenderer& renderer, const int fontId, const bool lineEndsWithHyphenatedWord,
    const bool suppressHyphenationRetry, const int firstLineIndent, const int16_t blockStartY, const int lineHeight) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  lastLineVisibleOffset_ = lastBreakAt < wordVisibleOffsets.size() ? wordVisibleOffsets[lastBreakAt] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Apply indent only to line 0 of the layout pass; firstLineIndent is already
  // 0 for continuation flushes (computed once in layoutAndExtractLines).
  const int lineIndent = (breakIndex == 0) ? firstLineIndent : 0;

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  // A token's width on this line: mid-line, but the first and last at their line-edge widths (CJK punctuation).
  const auto boxWidth = [&](const size_t wordIdx) {
    int width = wordWidths[lastBreakAt + wordIdx];
    if (wordIdx == 0) width += lineStartTrim_[lastBreakAt];
    if (wordIdx + 1 == lineWordCount) width += lineEndTrim_[lastBreakAt + wordIdx];
    return width;
  };

  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += boxWidth(wordIdx);
    // Count gaps: each word after the first creates a gap, unless it's a continuation.
    // Gaps before closing punctuation (. , ) » etc.) are excluded from justification
    // distribution so they stay at natural space width.
    const uint32_t firstCp = firstCodepoint(words[lastBreakAt + wordIdx]);
    const TokenBoundary::Boundary boundary = boundaries[lastBreakAt + wordIdx];
    if (wordIdx > 0 && !boundary.continues) {
      // A word gap or a CJK gap: both stretch when justified, only the word gap has a width.
      const bool beforeClosing = isClosingPunctuation(firstCp);
      if (!beforeClosing) actualGapCount++;
      if (TokenBoundary::hasSpace(boundary)) {
        // Inter-word gaps scale with the preceding word's effective size, matching wordStyles usage.
        totalNaturalGaps += static_cast<int>(
            spaceBefore(renderer, fontId, lastBreakAt + wordIdx) * wordScale(lastBreakAt + wordIdx - 1) + 0.5f);
      }
    } else if (wordIdx > 0) {
      // Non-breaking space tokens (" " with continues=true) are visible, stretchable spaces —
      // count them as justifiable gaps so justifyExtra is distributed to them too.
      if (words[lastBreakAt + wordIdx] == " ") {
        actualGapCount++;
      }
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps += static_cast<int>(
          kernBefore(renderer, fontId, lastBreakAt + wordIdx) * wordScale(lastBreakAt + wordIdx - 1) + 0.5f);
    }
  }

  // Calculate spacing (account for float zone narrowing + first-line indent)
  const int effectivePageWidth =
      widthForLine(static_cast<int>(breakIndex), lineHeight, blockStartY, pageWidth) - lineIndent;
  // A line is only truly last when it consumes all paragraph words.
  // During single-line retry we may temporarily pass a truncated break vector,
  // so relying only on breakIndex would incorrectly disable justification.
  const bool isLastLine = lineBreak == words.size();

  // For justified text, compute per-gap extra to distribute remaining space evenly.
  // Cap stretch to effectivePageWidth/8 per gap to prevent rivers in sparse lines.
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1)
                               ? std::min(spareSpace / static_cast<int>(actualGapCount), effectivePageWidth / 8)
                               : 0;

  // Calculate initial x position (first line starts at indent for left/justified text;
  // may be negative for hanging indents, e.g. margin-left:3em; text-indent:-1em).
  auto xpos = static_cast<int16_t>(lineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
  }

  // A narrowed CJK mark is drawn where its ink sits: ink in the right half of its em (an opening bracket) moves
  // left by what the mark lost, centred ink by half of it, ink in the left half stays (spec 5.2).
  const auto inkShift = [&](const size_t wordIdx, const int box) {
    const size_t i = lastBreakAt + wordIdx;
    const auto* p = reinterpret_cast<const unsigned char*>(words[i].c_str());
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || *p != 0 || CjkPunctuation::classOf(cp) == CjkPunctuation::Class::None) return 0;
    const int natural = measureWordWidth(renderer, wordFontId(i, fontId), words[i], wordStyles[i], false, wordScale(i));
    if (box >= natural) return 0;
    int inkLeft = 0;
    int inkWidth = 0;
    int advance = 0;
    if (!renderer.getGlyphInk(wordFontId(i, fontId), cp, wordStyles[i], inkLeft, inkWidth, advance) || advance <= 0) {
      return 0;
    }
    const int removed = natural - box;
    if (2 * inkLeft >= advance) return removed;
    if (2 * (inkLeft + inkWidth) <= advance) return 0;
    return removed / 2;
  };

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<int16_t>& lineXPos = lineXPosScratch_;
  lineXPos.clear();  // keeps the capacity: the point is to stop reallocating per line
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    const int box = boxWidth(wordIdx);
    lineXPos.push_back(static_cast<int16_t>(xpos - inkShift(wordIdx, box)));

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && boundaries[lastBreakAt + wordIdx + 1].continues;
    if (nextIsContinuation) {
      int advance = box;
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      advance += static_cast<int>(
          kernBefore(renderer, fontId, lastBreakAt + wordIdx + 1) * wordScale(lastBreakAt + wordIdx) + 0.5f);
      // Non-breaking space tokens are stretchable — expand them during justification like normal spaces.
      if (words[lastBreakAt + wordIdx] == " " && boundaries[lastBreakAt + wordIdx].continues &&
          blockStyle.alignment == CssTextAlign::Justify && !isLastLine) {
        advance += justifyExtra;
      }
      xpos += advance;
    } else {
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        const uint32_t nextFirstCp = firstCodepoint(words[lastBreakAt + wordIdx + 1]);
        // A CJK gap has no natural width; it only takes the justification share below.
        if (TokenBoundary::hasSpace(boundaries[lastBreakAt + wordIdx + 1])) {
          gap = static_cast<int>(
              spaceBefore(renderer, fontId, lastBreakAt + wordIdx + 1) * wordScale(lastBreakAt + wordIdx) + 0.5f);
        }
        // Don't stretch the gap before closing punctuation — it looks wrong with
        // extra space before ".", ")", "»" etc.
        const bool nextIsClosing = isClosingPunctuation(nextFirstCp);
        if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && !nextIsClosing) {
          gap += justifyExtra;
        }
      }
      xpos += box + gap;
    }
  }

  // Address the line's words in place. The source is left intact so retry paths can still
  // inspect/merge tokens, and TextBlock copies straight from here into its arena — stripping
  // soft hyphens as it goes — so a line costs one allocation instead of four.
  TextBlock::WordRange range;
  range.words = &words;
  range.styles = &wordStyles;
  range.sizes = &wordSizes;
  // Carried into the block so a reader-side consumer can tell one logical word from the
  // fragments layout split it into (bionic-reading halves, attached punctuation). Nothing on
  // the render path reads it; the xpos above already encodes the spacing.
  range.boundaries = &boundaries;
  range.fontTable = fontSlots_;
  range.first = lastBreakAt;
  range.count = lineWordCount;

  // The page-fit decision runs before the line exists (see beforeLine_): the parser may emit
  // the current page here and open the next one's arena block, so the block built just below
  // is allocated from the page it will actually land on.
  if (beforeLine_) {
    uint8_t maxPct = 100;
    for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
      maxPct = std::max(maxPct, wordSizes[lastBreakAt + wordIdx].pct);
    }
    beforeLine_(maxPct);
  }

  // TextBlock flattens the range into its arena on construct; on arena OOM the
  // block is invalid, so drop the line rather than render/serialize garbage.
  auto block = makeUniqueNoThrow<TextBlock>(range, lineXPos, blockStyle, lineArena_);
  if (!block || !block->valid()) {
    LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
    return LineProcessResult::Accepted;
  }
  return processLine(std::move(block), lineEndsWithHyphenatedWord, suppressHyphenationRetry);
}

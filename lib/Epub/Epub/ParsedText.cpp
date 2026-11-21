#include "ParsedText.h"

#include <BidiUtils.h>
#include <ComplexShaper.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <TextScan.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string_view>
#include <vector>

#include "CjkBreaks.h"
#include "TokenBoundary.h"
#include "hyphenation/HyphenationCommon.h"
#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
// Paragraph-level direction: scan the first N words to find base direction.
constexpr size_t RTL_PARAGRAPH_PROBE_WORDS = 3;
// Per-word: scan enough chars to see through leading neutrals (quotes, numbers)
// before giving up. 64 is a hedge for pathological cases like long numeric tokens.
constexpr int RTL_PER_WORD_PROBE_DEPTH = 64;
constexpr size_t MIN_JUSTIFY_GAPS = 1;

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string_view word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.data());
  const auto* const end = ptr + word.size();
  while (ptr < end) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
  return 0;
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string_view word) {
  for (size_t i = word.size(); i-- > 0;) {
    if ((static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) continue;
    const auto* ptr = reinterpret_cast<const unsigned char*>(word.data() + i);
    if (const uint32_t cp = utf8NextCodepoint(&ptr); cp != 0x00AD) return cp;
  }
  return 0;
}

bool containsSoftHyphen(const std::string_view word) { return word.find(SOFT_HYPHEN_UTF8) != std::string_view::npos; }

bool containsCjkBreakableCodepoint(const std::string_view text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
  for (const auto* const end = ptr + text.size(); ptr < end;) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (utf8IsCjkBreakable(cp)) {
      return true;
    }
  }
  return false;
}

uint32_t countCodepoints(const std::string_view text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
  const auto* const end = ptr + text.size();
  uint32_t count = 0;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

// Line-end split points inside a Hangul word, using the CJK boundary rules (no hyphen is drawn).
std::vector<Hyphenator::BreakInfo> hangulLineEndBreaks(const std::string& word) {
  std::vector<Hyphenator::BreakInfo> breaks;
  if (word.empty()) return breaks;
  breaks.reserve(word.size() / 6);
  const auto* const start = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* ptr = start;
  uint32_t prev = utf8NextCodepoint(&ptr);
  while (*ptr) {
    const size_t offset = static_cast<size_t>(ptr - start);
    const uint32_t cur = utf8NextCodepoint(&ptr);
    if ((utf8IsHangul(prev) || utf8IsHangul(cur)) && CjkBreaks::boundaryAllowsBreak(prev, cur)) {
      breaks.push_back({offset, false});
    }
    prev = cur;
  }
  return breaks;
}

// Scans text for explicit hyphens or dashes and returns byte offsets where a line break is permitted right after
// the hyphen/dash. Multi-character hyphen/dash runs are kept together as an indivisible unit.
std::vector<size_t> explicitHyphenBreakByteOffsets(const std::string& text, const bool attachToPrevious) {
  std::vector<size_t> breakOffsets;
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  const auto* const end = ptr + text.size();

  bool hasLeadingContent = attachToPrevious;

  while (ptr < end) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;

    if (TokenBoundary::allowsBreakAfterExplicitHyphen(cp)) {
      while (ptr < end) {
        const auto* const nextStart = ptr;
        const uint32_t nextCp = utf8NextCodepoint(&ptr);
        if (!TokenBoundary::allowsBreakAfterExplicitHyphen(nextCp)) {
          ptr = nextStart;
          break;
        }
      }

      if (hasLeadingContent && ptr < end) {
        breakOffsets.push_back(static_cast<size_t>(ptr - start));
      }
      hasLeadingContent = true;
    } else {
      hasLeadingContent = true;
    }
  }

  return breakOffsets;
}

constexpr size_t MAX_HYPHEN_WORD_BYTES = 200, FAST_MEASURE_BYTES = 156, MAX_DP_BREAKPOINTS = 400;
static_assert(MAX_HYPHEN_WORD_BYTES <= UINT8_MAX);
struct BreakPoint {
  uint16_t word, preW, sufW;
  uint8_t off, flags;  // bit 0: needsHyphen; bits 1..2: leftHang; bits 3..4: rightHang
  static uint8_t flg(bool h, uint8_t l, uint8_t r) { return static_cast<uint8_t>(h | ((l & 3) << 1) | ((r & 3) << 3)); }
};
constexpr uint32_t DP_INF = 4000000000U, DP_SAT = DP_INF - 1;
struct DpCell {
  uint32_t cost[2] = {};
  uint16_t next[2] = {};
};
static_assert(sizeof(BreakPoint) == 8 && sizeof(DpCell) == 12);
constexpr uint32_t satDem(uint64_t v) { return v >= DP_SAT ? DP_SAT : static_cast<uint32_t>(v); }
uint8_t hyphenClass(std::string_view w) {
  uint8_t mask = 0;
  const auto *p = reinterpret_cast<const unsigned char*>(w.data()), *e = p + w.size();
  for (size_t a = 0; p < e;)
    if (const uint32_t cp = utf8NextCodepoint(&p))
      mask |= static_cast<uint8_t>((cp == 0x00AD || (isAlphabetic(cp) && ++a >= 6)) | (utf8IsHangul(cp) ? 2 : 0) |
                                   (utf8IsCjkBreakable(cp) ? 4 : 0));
  return mask;
}
__attribute__((noinline)) bool appendSplitPrefix(WordStore& ws, std::string_view w, size_t off, bool h,
                                                 WordStore::StoredWord& out) {
  char buf[MAX_HYPHEN_WORD_BYTES + 4];
  if (off + 1 > sizeof(buf)) return false;
  memcpy(buf, w.data(), off);
  if (h) buf[off] = '-';
  return ws.append(buf, off + h, out);
}
std::vector<Hyphenator::BreakInfo> collectWordBreaks(const std::string& w, uint8_t hc, bool fb) {
  if (!(hc & 2)) return Hyphenator::breakOffsets(w, fb);
  auto out = hangulLineEndBreaks(w);
  for (const auto& b : (hc & 1) ? Hyphenator::breakOffsets(w, fb) : decltype(out){}) out.push_back(b);
  return out;
}
int maxLineShrinkPx(bool j, size_t g, size_t sg, int spW, int nw) {
  return j && sg && sg == g ? std::min((spW * 18) / 100, static_cast<int>(sg) * (nw > 1 ? (nw + 3) / 4 : 0)) : 0;
}
struct JustifyDistributor {
  int base = 0, sign = 0, rem = 0, idx = 0, n = 0, err = 0;
  static JustifyDistributor make(bool j, int sp, size_t g, size_t sg, int spW, int nw) {
    const int s = !j || g < MIN_JUSTIFY_GAPS ? 0 : (sp < 0 ? std::max(sp, -maxLineShrinkPx(true, g, sg, spW, nw)) : sp);
    const int gn = static_cast<int>(g), r = s ? s % gn : 0;
    return s ? JustifyDistributor{s / gn, (s > 0) - (s < 0), r < 0 ? -r : r, 0, gn, 0} : JustifyDistributor{};
  }
  int next() {
    if (idx++ >= n) return 0;
    if ((err += rem) < n) return base;
    err -= n;
    return base + sign;
  }
  int total() const { return base * n + sign * rem; }
};

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

constexpr int scaleSpace(const int advance, const uint8_t wordSpacingPercent) {
  return wordSpacingPercent == 100 ? advance : (advance * wordSpacingPercent + 50) / 100;
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
// `word` is usually NUL-terminated at data()[size()] (arena entries and
// std::string arguments), but focus-split candidates pass PREFIX views of a
// stored word. Probing data()[size()] is in-bounds for every caller (it is
// either the word's own NUL or a mid-word byte of the same arena entry) and
// routes unterminated views through the copying path so the C API never
// measures past the view.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string_view word,
                          const EpdFontFamily::Style style, const int8_t characterSpacing,
                          const uint8_t wordSpacingPercent, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return scaleSpace(renderer.getSpaceWidth(fontId, style), wordSpacingPercent);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen && word.data()[word.size()] == '\0') {
    return renderer.getTextAdvanceX(fontId, word.data(), style, characterSpacing);
  }
  if (word.size() <= FAST_MEASURE_BYTES) {
    char buf[FAST_MEASURE_BYTES + 4];
    size_t n = 0;
    for (size_t i = 0; i < word.size(); ++i) {
      if (hasSoftHyphen && i + 1 < word.size() && word[i] == '\xC2' && word[i + 1] == '\xAD' && ++i) continue;
      buf[n++] = word[i];
    }
    if (appendHyphen) buf[n++] = '-';
    buf[n] = '\0';
    return renderer.getTextAdvanceX(fontId, buf, style, characterSpacing);
  }

  std::string sanitized(word);
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style, characterSpacing);
}

// True when a token ends with a hyphen or dash that allows a line break after it. U+00AD is
// excluded because it renders as nothing, so only a hyphenation break -- which substitutes a
// visible '-' -- may use it; U+2011 because forbidding that break is its purpose.
bool endsWithBreakableHyphen(const std::string_view token) {
  if (token.empty()) return false;
  return TokenBoundary::allowsBreakAfterExplicitHyphen(lastCodepoint(token));
}

// Focus Reading renders the first `focusBoundary` bytes of a token bold and the rest at the
// token's own style; 0 means no emphasis. The bold run is at most 9 codepoints (see addWord),
// so it is copied to a stack buffer rather than a substring: this runs once per word during
// pagination and must not allocate.
constexpr size_t FOCUS_PREFIX_BUF_SIZE = 40;

// Bold prefix width plus kerning across the weight change. In LTR this is also
// the offset to the regular suffix; in RTL it contributes to the total word width.
uint16_t measureFocusPrefixAdvance(const GfxRenderer& renderer, const int fontId, const std::string_view word,
                                   const EpdFontFamily::Style style, const uint8_t focusBoundary,
                                   const int8_t characterSpacing) {
  char prefixBuf[FOCUS_PREFIX_BUF_SIZE];
  const size_t prefixLen = std::min<size_t>(focusBoundary, FOCUS_PREFIX_BUF_SIZE - 1);
  memcpy(prefixBuf, word.data(), prefixLen);
  prefixBuf[prefixLen] = '\0';

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  const auto* suffixPtr = reinterpret_cast<const unsigned char*>(word.data() + focusBoundary);
  const uint32_t suffixFirst = utf8NextCodepoint(&suffixPtr);
  const bool rtl = BidiUtils::startsWithRtl(prefixBuf);
  const int kerning =
      rtl ? renderer.getKerning(fontId, suffixFirst, lastCodepoint(prefixBuf), style, characterSpacing)
          : renderer.getKerning(fontId, lastCodepoint(prefixBuf), suffixFirst, boldStyle, characterSpacing);
  return static_cast<uint16_t>(renderer.getTextAdvanceX(fontId, prefixBuf, boldStyle, characterSpacing) + kerning);
}

// Advance width of a whole token, accounting for a bold focus prefix when it has one.
uint16_t measureFocusWordWidth(const GfxRenderer& renderer, const int fontId, const std::string_view word,
                               const EpdFontFamily::Style style, const uint8_t focusBoundary,
                               const int8_t characterSpacing, const uint8_t wordSpacingPercent,
                               const bool appendHyphen = false) {
  if (focusBoundary == 0) {
    return measureWordWidth(renderer, fontId, word, style, characterSpacing, wordSpacingPercent, appendHyphen);
  }
  if (focusBoundary >= word.size()) {
    // The bold run covers the whole token, as for a split candidate ending on the boundary.
    return measureWordWidth(renderer, fontId, word, static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD),
                            characterSpacing, wordSpacingPercent, appendHyphen);
  }
  // Through measureWordWidth (not getTextAdvanceX directly): `word` itself can
  // be an unterminated prefix view here, and the suffix view inherits that.
  const uint16_t suffixWidth = measureWordWidth(renderer, fontId, word.substr(focusBoundary), style, characterSpacing,
                                                wordSpacingPercent, appendHyphen);
  return measureFocusPrefixAdvance(renderer, fontId, word, style, focusBoundary, characterSpacing) + suffixWidth;
}

// RTL draws the regular suffix on the left and the bold prefix on the right.
uint16_t measureFocusRtlSplitAdvance(const GfxRenderer& renderer, const int fontId, const std::string_view word,
                                     const EpdFontFamily::Style style, const uint8_t boundary,
                                     const int8_t characterSpacing) {
  const auto suffix = word.substr(boundary);
  const int kerning = renderer.getKerning(fontId, firstCodepoint(suffix), lastCodepoint(word.substr(0, boundary)),
                                          style, characterSpacing);
  return measureWordWidth(renderer, fontId, suffix, style, characterSpacing, 100) + kerning;
}

// Focus boundaries for the two halves of a token split at `splitOffset`.
uint8_t focusBoundaryBefore(const uint8_t focusBoundary, const size_t splitOffset) {
  return static_cast<uint8_t>(std::min<size_t>(focusBoundary, splitOffset));
}
uint8_t focusBoundaryAfter(const uint8_t focusBoundary, const size_t splitOffset) {
  return focusBoundary > splitOffset ? static_cast<uint8_t>(focusBoundary - splitOffset) : 0;
}

uint8_t optHang(bool on, const GfxRenderer& r, int fontId, std::string_view tok, EpdFontFamily::Style st, uint8_t focus,
                bool left, bool addHyph = false) {
  if (!on || tok.empty()) return 0;
  const uint32_t cp = left ? firstCodepoint(tok) : (addHyph ? '-' : lastCodepoint(tok));
  const auto in = [&](std::u16string_view s) { return cp <= 0xFFFF && s.find(static_cast<char16_t>(cp)) != s.npos; };
  uint8_t pct = in(left ? u"\"'\u00AB\u2018\u201C\u2039" : u"-,.\"'\u00BB\u2010\u2011\u2019\u201D\u203A") ? 40 : 0;
  if (!pct && !left && in(u"\u2012\u2013\u2014\u2026")) pct = 25;
  if (!pct || containsCjkBreakableCodepoint(tok)) return 0;
  char cpBuf[5] = {};
  utf8AppendCodepoint(cpBuf, cp);
  if (left ? focus > 0 : focus >= tok.size()) st = static_cast<EpdFontFamily::Style>(st | EpdFontFamily::BOLD);
  const int adv = r.getTextAdvanceX(fontId, cpBuf, st, 0, BidiUtils::BidiBaseDir::LTR);
  return adv > 0 ? static_cast<uint8_t>(std::min(adv, std::clamp((adv * pct + 50) / 100, 1, 3))) : 0;
}

// Checks if a UTF-8 codepoint should be counted as part of a word for Focus Reading
bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;

  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

}  // namespace

uint32_t ParsedText::visibleOffsetBaseAt(const size_t wordIndex) const {
  uint32_t base = visibleOffsetBase;
  for (const auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex > wordIndex) break;
    base = rebase.base;
  }
  return base;
}

uint32_t ParsedText::visibleOffsetAt(const size_t wordIndex) const {
  if (wordIndex >= wordVisibleOffsetDeltas.size()) return 0;
  return visibleOffsetBaseAt(wordIndex) + wordVisibleOffsetDeltas[wordIndex];
}

uint32_t ParsedText::sourceOffsetAfter(const uint32_t start, const uint32_t renderedLength) const {
  uint32_t end = start + renderedLength;
  const auto* chunk =
      absorbedSourceCursor && start >= absorbedSourceCursorStart ? absorbedSourceCursor : absorbedSourceHead.get();
  while (chunk && chunk->base + chunk->deltas[chunk->count - 1] <= start) chunk = chunk->next.get();
  absorbedSourceCursor = chunk;
  absorbedSourceCursorStart = start;
  for (; chunk; chunk = chunk->next.get()) {
    const auto* first = chunk->deltas + chunk->begin;
    const auto* last = chunk->deltas + chunk->count;
    if (start >= chunk->base) first = std::upper_bound(first, last, start - chunk->base);
    for (; first != last; ++first) {
      if (chunk->base + *first > end) return end;
      ++end;
    }
  }
  return end;
}

bool ParsedText::recordAbsorbedSourceOffset(const uint32_t offset) {
  if (!absorbedSourceTail || absorbedSourceTail->count == 64 || offset < absorbedSourceTail->base ||
      offset - absorbedSourceTail->base > std::numeric_limits<uint16_t>::max()) {
    // About 140 bytes per chunk on C3; sparse and paragraph-lived, with no large growth copy.
    auto chunk = makeUniqueNoThrow<AbsorbedSourceChunk>();
    if (!chunk) {
      LOG_ERR("PTX", "OOM: NFC source offsets (%u bytes)", static_cast<unsigned>(sizeof(AbsorbedSourceChunk)));
      droppedWords = true;
      return false;
    }
    chunk->base = offset;
    auto* tail = chunk.get();
    if (absorbedSourceTail) {
      absorbedSourceTail->next = std::move(chunk);
    } else {
      absorbedSourceHead = std::move(chunk);
    }
    absorbedSourceTail = tail;
  }
  absorbedSourceTail->deltas[absorbedSourceTail->count++] = static_cast<uint16_t>(offset - absorbedSourceTail->base);
  return true;
}

void ParsedText::retireAbsorbedSourceOffsets(const uint32_t remainingStart) {
  absorbedSourceCursor = nullptr;
  while (absorbedSourceHead &&
         absorbedSourceHead->base + absorbedSourceHead->deltas[absorbedSourceHead->count - 1] <= remainingStart) {
    auto retired = std::move(absorbedSourceHead);
    absorbedSourceHead = std::move(retired->next);
  }
  if (!absorbedSourceHead) {
    absorbedSourceTail = nullptr;
  } else if (remainingStart >= absorbedSourceHead->base) {
    absorbedSourceHead->begin =
        std::upper_bound(absorbedSourceHead->deltas, absorbedSourceHead->deltas + absorbedSourceHead->count,
                         remainingStart - absorbedSourceHead->base) -
        absorbedSourceHead->deltas;
  }
}

void ParsedText::pushVisibleOffset(const uint32_t offset) {
  uint32_t base = visibleOffsetBase;
  if (wordVisibleOffsetDeltas.empty()) {
    visibleOffsetBase = offset;
    base = offset;
  } else if (!visibleOffsetRebases.empty()) {
    base = visibleOffsetRebases.back().base;
  }

  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    visibleOffsetRebases.push_back({wordVisibleOffsetDeltas.size(), offset});
    base = offset;
  }
  wordVisibleOffsetDeltas.push_back(static_cast<uint16_t>(offset - base));
}

void ParsedText::insertVisibleOffset(const size_t wordIndex, const uint32_t offset) {
  const uint32_t base = wordIndex > 0 ? visibleOffsetBaseAt(wordIndex - 1) : visibleOffsetBase;
  for (auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex >= wordIndex) rebase.wordIndex++;
  }

  uint32_t insertionBase = base;
  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    const auto rebaseIt = std::find_if(visibleOffsetRebases.begin(), visibleOffsetRebases.end(),
                                       [wordIndex](const auto& rebase) { return rebase.wordIndex > wordIndex; });
    visibleOffsetRebases.insert(rebaseIt, {wordIndex, offset});
    insertionBase = offset;
  }
  wordVisibleOffsetDeltas.insert(wordVisibleOffsetDeltas.begin() + wordIndex,
                                 static_cast<uint16_t>(offset - insertionBase));
}

void ParsedText::eraseVisibleOffsetPrefix(const size_t count) {
  if (count >= wordVisibleOffsetDeltas.size()) {
    wordVisibleOffsetDeltas.clear();
    visibleOffsetRebases.clear();
    visibleOffsetBase = 0;
    retireAbsorbedSourceOffsets(UINT32_MAX);
    return;
  }

  const uint32_t remainingStart = visibleOffsetAt(count);
  retireAbsorbedSourceOffsets(remainingStart);
  const uint32_t newBase = visibleOffsetBaseAt(count);
  wordVisibleOffsetDeltas.erase(wordVisibleOffsetDeltas.begin(), wordVisibleOffsetDeltas.begin() + count);
  size_t writeIndex = 0;
  for (auto rebase : visibleOffsetRebases) {
    if (rebase.wordIndex <= count) continue;
    rebase.wordIndex -= count;
    visibleOffsetRebases[writeIndex++] = rebase;
  }
  visibleOffsetRebases.resize(writeIndex);
  visibleOffsetBase = newBase;
}

bool ParsedText::storeWord(const std::string_view text, WordStore::StoredWord& out) {
  if (wordStore.append(text.data(), text.size(), out)) return true;
  if (!droppedWords) {
    LOG_ERR("PTX", "OOM: dropping paragraph text (arena chunk alloc failed)");
  }
  droppedWords = true;
  return false;
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint32_t visibleTextOffset, const uint8_t linkId,
                         const uint8_t sizeSlot, const bool noSpaceBefore) {
  if (word.empty()) return;

  // The device fonts carry no combining-mark positioning, so EPUB text stored in NFD
  // (a base letter followed by separate combining accents -- common for Vietnamese,
  // and used for many EPUB <h1> chapter headings) renders with the marks detached or
  // misplaced. Compose to NFC here, the single funnel every word passes through, so a
  // precomposed glyph is used instead. This runs once per word at layout time (the
  // result is cached in the section file) and is a cheap no-op for mark-free text.
  struct SourceContext {
    ParsedText* text;
    uint32_t start;
  } sourceContext{this, visibleTextOffset};
  word = utf8ComposeNfc(
      word,
      [](const uint32_t offset, void* context) {
        const auto& source = *static_cast<SourceContext*>(context);
        return source.text->recordAbsorbedSourceOffset(source.start + offset);
      },
      &sourceContext);
  if (word.empty()) return;

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }
  const Utf8TextScan scan = utf8ScanText(word.c_str());
  hasComplexWord = hasComplexWord || scan.complexScript;
  const bool wordStartsRtl =
      !hasRtlWord && scan.rtl && BidiUtils::startsWithRtl(word.c_str(), RTL_PER_WORD_PROBE_DEPTH);

  // All token pushes funnel through here: the arena append is the only
  // fallible step, and a failed append drops the token without touching the
  // parallel arrays (they must stay in lockstep with words).
  const auto pushStyledToken = [&](std::string_view token, const EpdFontFamily::Style style, const bool continues,
                                   const bool noSpaceBefore, const uint8_t focusBoundary, const uint32_t tokenOffset,
                                   const bool padRuby) {
    WordStore::StoredWord stored;
    if (!storeWord(token, stored)) return;
    words.push_back(stored);
    // Small caps share the style byte's high bit with the discretionary-hyphen flag, so it
    // rides in the size slot instead.
    wordStyles.push_back(static_cast<EpdFontFamily::Style>(style & ~EpdFontFamily::SMALL_CAPS));
    wordContinues.push_back(continues);
    wordNoSpaceBefore.push_back(noSpaceBefore);
    wordFocusBoundary.push_back(focusBoundary);
    wordLinkIds.push_back(linkId);
    wordSizeSlots.push_back(
        static_cast<uint8_t>(sizeSlot | ((style & EpdFontFamily::SMALL_CAPS) ? TextBlock::SMALL_CAPS_SLOT_FLAG : 0)));
    pushVisibleOffset(tokenOffset);
    if (padRuby && !rubyTexts.empty()) {
      rubyTexts.push_back("");
    }
  };

  const auto pushToken = [&](std::string_view token, const bool continues, const bool noSpaceBefore,
                             const uint8_t focusBoundary, const uint32_t tokenOffset) {
    pushStyledToken(token, baseStyle, continues, noSpaceBefore, focusBoundary, tokenOffset, true);
  };

  bool effectiveAttachToPrevious = attachToPrevious;
  bool effectiveNoSpaceBefore = noSpaceBefore;
  // Only a glued token (attachToPrevious == true, i.e. no whitespace separated it from the
  // previous one in the source) may be turned into a gap-less break opportunity. When real
  // whitespace separated the two words, that space is content and must be rendered: Korean
  // is a space-delimited script written in Hangul, which utf8IsCjkBreakable() covers.
  if (attachToPrevious && !words.empty() && endsWithBreakableHyphen(wordStore.view(words.back()))) {
    effectiveAttachToPrevious = true;
    effectiveNoSpaceBefore = true;
  } else if (attachToPrevious && !words.empty() &&
             CjkBreaks::hasBreakOpportunityBetween(lastCodepoint(wordStore.view(words.back())), firstCodepoint(word))) {
    effectiveAttachToPrevious = false;
    effectiveNoSpaceBefore = true;
  }

  // Bulk-reserve the per-token parallel arrays before a burst of pushes so they
  // don't repeatedly double. Only the std::vector arrays are reserved: words and
  // rubyTexts are std::deque (chunked growth, no reserve()/capacity() and no large
  // contiguous reallocation to avoid). wordStyles' capacity gauges them all since
  // pushToken() keeps every array in lockstep.
  const auto ensureTokenCapacity = [&](const size_t additionalTokens) {
    if (additionalTokens == 0) return;
    const size_t requiredSize = words.size() + additionalTokens;
    if (wordStyles.capacity() >= requiredSize) return;

    size_t newCapacity = wordStyles.capacity() < 16 ? 16 : wordStyles.capacity();
    while (newCapacity < requiredSize) {
      newCapacity *= 2;
    }

    wordStyles.reserve(newCapacity);
    wordContinues.reserve(newCapacity);
    wordNoSpaceBefore.reserve(newCapacity);
    wordFocusBoundary.reserve(newCapacity);
    wordLinkIds.reserve(newCapacity);
    wordSizeSlots.reserve(newCapacity);
    wordVisibleOffsetDeltas.reserve(newCapacity);
  };

  if (auto breakOffsets = CjkBreaks::characterBreakByteOffsets(word); !breakOffsets.empty()) {
    // CJK-heavy paragraphs can push hundreds of tiny tokens quickly when CSS toggles
    // inline styles. Reserve once up front to avoid repeated vector growth reallocations.
    ensureTokenCapacity(breakOffsets.size() + 1);
    bool firstToken = true;
    size_t tokenStart = 0;
    uint32_t tokenVisibleOffset = visibleTextOffset;
    for (const size_t breakOffset : breakOffsets) {
      if (breakOffset <= tokenStart || breakOffset > word.size()) continue;
      const std::string_view token(word.data() + tokenStart, breakOffset - tokenStart);
      const bool attach = firstToken ? effectiveAttachToPrevious
                                     : (!words.empty() && endsWithBreakableHyphen(wordStore.view(words.back())));
      pushToken(token, attach, firstToken ? effectiveNoSpaceBefore : true,
                /*focusBoundary=*/0, tokenVisibleOffset);
      tokenVisibleOffset = sourceOffsetAfter(tokenVisibleOffset, countCodepoints(token));
      firstToken = false;
      tokenStart = breakOffset;
    }
    if (tokenStart < word.size()) {
      const bool attach = firstToken ? effectiveAttachToPrevious
                                     : (!words.empty() && endsWithBreakableHyphen(wordStore.view(words.back())));
      pushToken(std::string_view(word).substr(tokenStart), attach, firstToken ? effectiveNoSpaceBefore : true,
                /*focusBoundary=*/0, tokenVisibleOffset);
    }
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  if (containsCjkBreakableCodepoint(word)) {
    pushToken(word, effectiveAttachToPrevious, effectiveNoSpaceBefore, /*focusBoundary=*/0, visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // Already-bold text should stay fully bold; focus splitting would make its suffix regular later.
  // Complex-script words never take a focus split: each half would shape on its own.
  if (!this->focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0 || scan.complexScript) {
    if (auto breakOffsets = explicitHyphenBreakByteOffsets(word, effectiveAttachToPrevious); !breakOffsets.empty()) {
      ensureTokenCapacity(breakOffsets.size() + 1);
      bool firstToken = true;
      size_t tokenStart = 0;
      uint32_t tokenVisibleOffset = visibleTextOffset;
      for (const size_t breakOffset : breakOffsets) {
        if (breakOffset <= tokenStart || breakOffset > word.size()) continue;
        const std::string_view token(word.data() + tokenStart, breakOffset - tokenStart);
        pushToken(token, firstToken ? effectiveAttachToPrevious : true, firstToken ? effectiveNoSpaceBefore : true,
                  /*focusBoundary=*/0, tokenVisibleOffset);
        tokenVisibleOffset = sourceOffsetAfter(tokenVisibleOffset, countCodepoints(token));
        firstToken = false;
        tokenStart = breakOffset;
      }
      if (tokenStart < word.size()) {
        pushToken(std::string_view(word).substr(tokenStart), firstToken ? effectiveAttachToPrevious : true,
                  firstToken ? effectiveNoSpaceBefore : true, /*focusBoundary=*/0, tokenVisibleOffset);
      }
      if (wordStartsRtl) {
        hasRtlWord = true;
      }
      return;
    }

    pushToken(word, effectiveAttachToPrevious, effectiveNoSpaceBefore, /*focusBoundary=*/0, visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // --- FOCUS READING LOGIC BELOW ---

  // Worst case: a segment boundary on each byte (highly punctuated UTF-8 text).
  ensureTokenCapacity(word.length());

  // Lambda helper to process and push individual sub-segments of the string
  // Use std::string_view to avoid heap allocations when slicing
  auto processSegment = [&](std::string_view segment, bool isWord, bool attach, bool noSpaceBefore) {
    const unsigned char* wordBegin = reinterpret_cast<const unsigned char*>(word.data());
    const unsigned char* segmentBegin = reinterpret_cast<const unsigned char*>(segment.data());
    uint32_t segmentLength = 0;
    const unsigned char* offsetPtr = wordBegin;
    while (offsetPtr < segmentBegin) {
      utf8NextCodepoint(&offsetPtr);
      segmentLength++;
    }
    const uint32_t segmentOffset = sourceOffsetAfter(visibleTextOffset, segmentLength);
    if (!isWord) {
      // Punctuation and Numbers stay regular
      pushStyledToken(segment, baseStyle, attach, noSpaceBefore, /*focusBoundary=*/0, segmentOffset, false);
    } else {
      size_t charCount = 0;
      const unsigned char* countPtr = reinterpret_cast<const unsigned char*>(segment.data());
      const unsigned char* countEnd = countPtr + segment.length();

      while (countPtr < countEnd) {
        utf8NextCodepoint(&countPtr);
        charCount++;
      }

      // Target 45% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation
      constexpr size_t FOCUS_READING_PERCENT = 45;
      size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
      targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);

      if (targetBoldChars >= charCount) {
        // Whole segment is bold - no suffix split needed
        pushStyledToken(segment, static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD), attach,
                        noSpaceBefore, /*focusBoundary=*/0, segmentOffset, false);
      } else {
        countPtr = reinterpret_cast<const unsigned char*>(segment.data());
        for (size_t i = 0; i < targetBoldChars; ++i) {
          utf8NextCodepoint(&countPtr);
        }
        size_t splitByteOffset = countPtr - reinterpret_cast<const unsigned char*>(segment.data());

        // One token carrying the emphasis as a byte boundary, so the word stays whole for the
        // hyphenator and the line breaker. The renderer applies BOLD to bytes [0, splitByteOffset).
        pushStyledToken(segment, baseStyle, attach, noSpaceBefore,
                        static_cast<uint8_t>(std::min<size_t>(splitByteOffset, 255)), segmentOffset, false);
      }
    }
  };

  // Tokenize the string by alternating states (Word vs. Non-Word)
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* end = ptr + word.length();

  const unsigned char* segmentStart = ptr;
  uint32_t firstCp = utf8NextCodepoint(&ptr);  // Consume the first char to determine initial state
  bool inWordSegment = isWordCharacter(firstCp);

  bool isFirstSegment = true;

  while (ptr < end) {
    const unsigned char* currentCpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    bool isWordChar = isWordCharacter(cp);

    // Whenever the character type flips, slice off the segment we just completed and process it
    if (isWordChar != inWordSegment) {
      size_t segmentLen = currentCpStart - segmentStart;
      std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);

      // Only the very first segment inherits the original attachToPrevious flag.
      // Every subsequent segment glues seamlessly to the prefix. After a visible explicit-hyphen
      // character, continues=true + noSpaceBefore=true records a breakable attachment: it may wrap,
      // but when it stays on the line it receives kerning only, never a space or justification.
      const bool breakAfterPrev =
          !isFirstSegment && !words.empty() && endsWithBreakableHyphen(wordStore.view(words.back()));
      processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                     isFirstSegment ? effectiveNoSpaceBefore : breakAfterPrev);

      // Setup for the next segment
      segmentStart = currentCpStart;
      inWordSegment = isWordChar;
      isFirstSegment = false;
    }
  }

  // Process the final remaining segment
  size_t segmentLen = end - segmentStart;
  std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);
  const bool breakAfterPrev =
      !isFirstSegment && !words.empty() && endsWithBreakableHyphen(wordStore.view(words.back()));
  processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                 isFirstSegment ? effectiveNoSpaceBefore : breakAfterPrev);
  if (wordStartsRtl) {
    hasRtlWord = true;
  }
}

uint8_t ParsedText::addLinkTarget(const char* href) {
  if (!href || href[0] == '\0' || strnlen(href, FOOTNOTE_HREF_LEN) >= FOOTNOTE_HREF_LEN ||
      linkTargets.size() >= UINT8_MAX) {
    return 0;
  }
  linkTargets.emplace_back(href);
  return static_cast<uint8_t>(linkTargets.size());
}

bool ParsedText::linkTargetMatches(const uint8_t linkId, const char* href) const {
  return linkId > 0 && linkId <= linkTargets.size() && href && linkTargets[linkId - 1] == href;
}

void ParsedText::setRubyForWordAt(size_t index, const std::string& ruby) {
  if (index >= words.size()) return;
  if (rubyTexts.size() <= index) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[index] = ruby;
}

void ParsedText::setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby) {
  if (startIndex >= words.size()) return;
  if (rubyTexts.size() <= startIndex) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[startIndex] = ruby;
  for (size_t i = 1; i < count; i++) {
    size_t idx = startIndex + i;
    if (idx >= words.size()) break;
    if (rubyTexts.size() <= idx) {
      rubyTexts.resize(words.size());
    }
    rubyTexts[idx] = "";
    wordStyles[idx] =
        static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(wordStyles[idx]) | EpdFontFamily::RUBY_CONTINUE);
    wordContinues[idx] = true;       // Prevent page breaker from splitting the Group Ruby!
    wordNoSpaceBefore[idx] = false;  // Ensure allowsBreak returns false!
  }
}

void ParsedText::ensureRubyCapacity() {
  // No-op: rubyTexts is a std::deque (chunked growth, no capacity to pre-reserve
  // and no large contiguous reallocation to avoid). Kept for call-site stability.
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine, const GfxRenderer& renderer, const int fontId) const {
  if (firstLineConsumed || !isFirstLine || !isNaturalAlign) {
    return 0;
  }
  if (blockStyle.textIndentDefined && blockStyle.textIndent < 0) return blockStyle.textIndent;
  if (paragraphIndentSpaces == 0) return 0;
  return scaleSpace(renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR), wordSpacingPercent) * paragraphIndentSpaces;
}
// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                                       const bool includeLastLine, const int8_t characterSpacing,
                                       const uint8_t wordSpacingPercent, const size_t maxLines,
                                       const bool* stopRequested) {
  if (words.empty() || maxLines == 0 || (stopRequested && *stopRequested)) {
    return;
  }
  // Measuring words and flattening lines both shape complex-script runs; the
  // memo makes the second pass reuse the first.
  const GfxRenderer::ShapingMemoScope shapingMemo(hasComplexWord);
  // Stamped here rather than at construction: the parser replaces blockStyle as CSS resolves.
  blockStyle.characterSpacing = characterSpacing;
  this->wordSpacingPercent = wordSpacingPercent;

  // Per-paragraph RTL auto-detection: only when CSS/HTML didn't explicitly set direction.
  // Explicit dir="ltr" must be respected and not overridden by content heuristic.
  if (!blockStyle.directionDefined && hasRtlWord) {
    // Check the first few words for RTL letter codepoints (no heap allocation).
    const size_t wordsToScan = std::min(words.size(), RTL_PARAGRAPH_PROBE_WORDS);
    for (size_t i = 0; i < wordsToScan; ++i) {
      if (BidiUtils::startsWithRtl(wordStore.cstr(words[i]), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
        blockStyle.isRtl = true;
        break;
      }
    }
  }

  isNaturalAlign =
      blockStyle.alignment == CssTextAlign::Justify ||
      (blockStyle.isRtl ? blockStyle.alignment == CssTextAlign::Right : blockStyle.alignment == CssTextAlign::Left);

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  // Every size in the paragraph measures through its own font: the block font plus any
  // inline-size slots resolved by the parser.
  int32_t paragraphFonts[MAX_SIZE_SLOTS] = {fontId};
  size_t paragraphFontCount = 1;
  for (uint8_t slot = 1; slot < sizeSlotCount; ++slot) {
    const int32_t slotFont = sizeSlotFontIds[slot];
    if (slotFont != 0 && std::find(paragraphFonts, paragraphFonts + paragraphFontCount, slotFont) ==
                             paragraphFonts + paragraphFontCount) {
      paragraphFonts[paragraphFontCount++] = slotFont;
    }
  }
  const bool anySdFont = std::any_of(paragraphFonts, paragraphFonts + paragraphFontCount,
                                     [&renderer](const int32_t font) { return renderer.isSdCardFont(font); });
  if (anySdFont) {
    // Style mask: only ask the SD font to load advances for styles actually
    // used in this paragraph. Style index is the low two bits (regular/bold/
    // italic/bold-italic); the underline bit is irrelevant to advance metrics.
    uint8_t styleMask = 0;
    for (auto s : wordStyles) {
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
    }
    if (focusReadingEnabled) styleMask |= static_cast<uint8_t>((styleMask & 0x05u) << 1);
    if (styleMask == 0) styleMask = 0x01;  // defensive: regular only
    const bool includeSmallCaps = std::any_of(wordSizeSlots.begin(), wordSizeSlots.end(), [](const uint8_t slot) {
      return (slot & TextBlock::SMALL_CAPS_SLOT_FLAG) != 0;
    });
    // Hand the arena chunks over as packed NUL-separated word runs. Two small
    // pointer tables (~8 B per live chunk) instead of per-word iteration.
    std::vector<const char*> segments;
    std::vector<size_t> segmentLens;
    segments.reserve(wordStore.chunkCount());
    segmentLens.reserve(wordStore.chunkCount());
    for (size_t i = 0; i < wordStore.chunkCount(); ++i) {
      const char* data = wordStore.chunkData(i);
      if (!data) continue;  // retired chunk
      segments.push_back(data);
      segmentLens.push_back(wordStore.chunkUsed(i));
    }
    for (size_t i = 0; i < paragraphFontCount; ++i) {
      if (!renderer.isSdCardFont(paragraphFonts[i])) continue;
      renderer.ensureSdCardFontReady(paragraphFonts[i], segments.data(), segmentLens.data(), segments.size(),
                                     words.size() > 1, hyphenationEnabled, styleMask, includeSmallCaps);
    }
  }

  const int pageWidth = viewportWidth;
  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (maxLines != SIZE_MAX) {
    // Bounded layout must leave words beyond the emitted lines unsplit for a later width.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues,
                                                   wordNoSpaceBefore, maxLines);
  } else {
    lineBreakIndices =
        computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, includeLastLine);
  }
  const bool allLinesComputed = lineBreakIndices.back() == words.size();
  const size_t lineCount = lineBreakIndices.size() - (!includeLastLine && allLinesComputed ? 1 : 0);

  const bool endIsKnown = includeLastLine && allLinesComputed;
  for (size_t i = 0; i < lineCount; ++i) {
    if (stopRequested && *stopRequested) return;
    linesAfterCurrent = endIsKnown ? static_cast<int>(lineCount - 1 - i) : -1;
    extractLine(i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, lineBreakIndices, processLine, renderer,
                fontId);
  }
  linesAfterCurrent = -1;

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    firstLineConsumed = true;
    paragraphStartEmitted = true;

    const size_t consumed = lineBreakIndices[lineCount - 1];
    for (size_t i = 0; i < consumed; ++i) {
      wordStore.release(words[i]);  // retires arena chunks as lines are consumed
    }
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + consumed);
    wordFocusBoundary.erase(wordFocusBoundary.begin(), wordFocusBoundary.begin() + consumed);
    wordLinkIds.erase(wordLinkIds.begin(), wordLinkIds.begin() + consumed);
    wordSizeSlots.erase(wordSizeSlots.begin(), wordSizeSlots.begin() + consumed);
    eraseVisibleOffsetPrefix(consumed);
    if (!rubyTexts.empty()) {
      const size_t rtConsumed = std::min(consumed, rubyTexts.size());
      rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rtConsumed);
    }
  }
}

// The first word of a line may have its ruby characters wider than the word (the base text). In that case, we need to
// move the base text to the right a bit so that ruby text doesn't overflow the left border, and it is still centered
// over the base text. This function calculates how much we need to move the base text to the right.
int ParsedText::calculateRubyExtraStartOffset(const size_t wordIdx, const size_t maxWordIdx,
                                              const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || wordIdx >= rubyTexts.size() || rubyTexts[wordIdx].empty() ||
      (wordStyles[wordIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  size_t groupWordCount = 1;
  while (wordIdx + groupWordCount < maxWordIdx &&
         (wordStyles[wordIdx + groupWordCount] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    groupWordCount++;
  }
  int groupActualWidth = 0;
  for (size_t k = 0; k < groupWordCount; ++k) {
    groupActualWidth += measureWordWidth(renderer, fontId, wordAt(wordIdx + k), getWordStyleAt(wordIdx + k),
                                         blockStyle.characterSpacing, wordSpacingPercent);
  }
  const int rubyWidth =
      renderer.getTextAdvanceX(fontId, rubyTexts[wordIdx].c_str(), EpdFontFamily::SUP, blockStyle.characterSpacing);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  // The ruby extends past the first base character by 1 unit (1 / 2M of the difference).
  // At the start of a line there is no preceding character to absorb overhang, so this
  // 1 unit must be reserved as a visual indent so ruby does not overflow the left margin.
  return (rubyWidth - groupActualWidth) / (2 * static_cast<int>(groupWordCount));
}

// The last ruby group on a line may have its ruby characters wider than the group's base text.
// The right unit of overhang protrudes past the last base character. This function returns
// the amount of right-margin space that must be reserved so the ruby does not overflow the right
// border. It mirrors calculateRubyExtraStartOffset.
int ParsedText::calculateRubyExtraEndOffset(const size_t lineStartIdx, const size_t lineBreakIdx,
                                            const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || lineBreakIdx == 0 || lineStartIdx >= lineBreakIdx) {
    return 0;
  }

  // Walk backwards from the last word to find the leader of the last ruby group on the line.
  size_t leaderIdx = lineBreakIdx - 1;
  while (leaderIdx > lineStartIdx && (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    leaderIdx--;
  }

  // leaderIdx must be a ruby group leader (non-empty ruby, no RUBY_CONTINUE flag).
  if (leaderIdx >= rubyTexts.size() || rubyTexts[leaderIdx].empty() ||
      (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  // Measure the group.
  int groupActualWidth = 0;
  for (size_t k = leaderIdx; k < lineBreakIdx; ++k) {
    groupActualWidth += measureWordWidth(renderer, fontId, wordAt(k), getWordStyleAt(k), blockStyle.characterSpacing,
                                         wordSpacingPercent);
  }
  const int rubyWidth =
      renderer.getTextAdvanceX(fontId, rubyTexts[leaderIdx].c_str(), EpdFontFamily::SUP, blockStyle.characterSpacing);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  const int m = static_cast<int>(lineBreakIdx - leaderIdx);
  const int delta = rubyWidth - groupActualWidth;
  return delta - (delta * (2 * m - 1)) / (2 * m);
}

int ParsedText::wordFontId(const size_t wordIndex, const int blockFontId) const {
  const uint8_t slot = wordSizeSlots[wordIndex] & ~TextBlock::SMALL_CAPS_SLOT_FLAG;
  return slot == 0 || sizeSlotFontIds[slot] == 0 ? blockFontId : sizeSlotFontIds[slot];
}

uint8_t ParsedText::sizeSlotFor(const float scale) {
  uint8_t nearest = 1;
  for (uint8_t slot = 1; slot < sizeSlotCount; ++slot) {
    if (std::fabs(sizeSlotScales[slot] - scale) < 0.01f) return slot;
    if (std::fabs(sizeSlotScales[slot] - scale) < std::fabs(sizeSlotScales[nearest] - scale)) nearest = slot;
  }
  if (sizeSlotCount < MAX_SIZE_SLOTS) {
    sizeSlotScales[sizeSlotCount] = scale;
    return sizeSlotCount++;
  }
  return nearest;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureFocusWordWidth(renderer, wordFontId(i, fontId), wordAt(i), getWordStyleAt(i),
                                               wordFocusBoundary[i], blockStyle.characterSpacing, wordSpacingPercent));
  }

  // Adjust widths for ruby groups to comply with JLReq standards
  if (!rubyTexts.empty()) {
    struct RubyGroupInfo {
      size_t start;
      size_t count;
      int baseWidth;
      int rubyWidth;
      int leftStickOut;
      int rightStickOut;
      int firstCharWidth;
      int lastCharWidth;
    };

    std::vector<RubyGroupInfo> groups;
    for (size_t i = 0; i < words.size(); ++i) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyles[i] & EpdFontFamily::RUBY_CONTINUE) == 0) {
        RubyGroupInfo g;
        g.start = i;
        g.baseWidth = wordWidths[i];
        g.count = 1;
        while (i + g.count < words.size() && (wordStyles[i + g.count] & EpdFontFamily::RUBY_CONTINUE) != 0) {
          g.baseWidth += wordWidths[i + g.count];
          g.count++;
        }
        g.rubyWidth =
            renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP, blockStyle.characterSpacing);

        const int delta = g.rubyWidth - g.baseWidth;
        if (delta > 0) {
          const int totalUnits = 2 * static_cast<int>(g.count);
          g.leftStickOut = delta / totalUnits;
          g.rightStickOut = delta - (delta * (totalUnits - 1)) / totalUnits;
        } else {
          g.leftStickOut = 0;
          g.rightStickOut = 0;
        }

        char firstChar[5] = {}, lastChar[5] = {};
        utf8AppendCodepoint(firstChar, firstCodepoint(rubyTexts[i]));
        utf8AppendCodepoint(lastChar, lastCodepoint(rubyTexts[i]));

        g.firstCharWidth = renderer.getTextAdvanceX(fontId, firstChar, EpdFontFamily::SUP, blockStyle.characterSpacing);
        g.lastCharWidth = renderer.getTextAdvanceX(fontId, lastChar, EpdFontFamily::SUP, blockStyle.characterSpacing);

        groups.push_back(g);
        i += g.count - 1;
      }
    }

    // Adjust widths based on adjacent characters and group-to-group spacing
    for (size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
      const auto& g = groups[gIdx];
      const size_t nextIdx = g.start + g.count;
      const int delta = g.rubyWidth - g.baseWidth;

      // 1. Spacing between base characters inside the group (if M >= 2 and ruby is wider)
      if (g.count >= 2 && delta > 0) {
        const int totalUnits = 2 * static_cast<int>(g.count);
        int prevCumulative = g.leftStickOut;
        for (size_t k = 0; k + 1 < g.count; ++k) {
          const int targetUnits = 1 + 2 * (static_cast<int>(k) + 1);
          const int nextCumulative = (delta * targetUnits) / totalUnits;
          wordWidths[g.start + k] += (nextCumulative - prevCumulative);
          prevCumulative = nextCumulative;
        }
      }

      const bool prevIsAdjacentRuby = (gIdx > 0 && groups[gIdx - 1].start + groups[gIdx - 1].count == g.start);
      const bool nextIsAdjacentRuby = (gIdx + 1 < groups.size() && groups[gIdx + 1].start == nextIdx);

      // 2. Preceding character (left overhang into regular text)
      if (g.start > 0 && !prevIsAdjacentRuby && g.leftStickOut > 0) {
        // Chromium would use something like `wordWidths[g.start - 1] / 2`, but
        // when two text blocks have ruby annotations twice as wide as their base
        // text and are separated by a half-width Latin letter, as in
        // `<ruby>諺<rt>ことわざ</rt></ruby>L<ruby>諺<rt>ことわざ</rt></ruby>`,
        // the two ruby annotation blocks would touch, potentially causing readers
        // to misread them. Chromium has additional code to handle this case, but
        // I want to avoid adding that complexity, so I use
        // `wordWidths[g.start - 1] / 4` instead of `/ 2`. A quarter of a kana's
        // width in the base text is equivalent to half a kana's width in the ruby
        // annotation, which is why I chose 4 as the divisor.
        const int maxLeftOverhang = std::min(g.firstCharWidth / 2, wordWidths[g.start - 1] / 4);
        if (g.leftStickOut > maxLeftOverhang) {
          wordWidths[g.start - 1] += (g.leftStickOut - maxLeftOverhang);
        }
      }

      // 3. Succeeding character (adjacent ruby collision or right overhang into regular text)
      if (nextIdx < words.size()) {
        if (nextIsAdjacentRuby) {
          const int collision = g.rightStickOut + groups[gIdx + 1].leftStickOut;
          if (collision > 0) {
            wordWidths[g.start + g.count - 1] += collision;
          }
        } else if (g.rightStickOut > 0) {
          const int maxRightOverhang = std::min(g.lastCharWidth / 2, wordWidths[nextIdx] / 4);
          if (g.rightStickOut > maxRightOverhang) {
            wordWidths[g.start + g.count - 1] += (g.rightStickOut - maxRightOverhang);
          }
        }
      }
    }
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec, const bool incLast) {
  if (words.empty()) {
    return {};
  }

  std::unique_ptr<BreakPoint[]> bps;
  const auto fallback = [&](size_t bytes) {
    bps.reset();
    if (bytes) LOG_ERR("PTX", "OOM: DP (%u B)", static_cast<unsigned>(bytes));
    return computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, continuesVec, noSpaceBeforeVec,
                                       SIZE_MAX);
  };
  if (words.size() >= MAX_DP_BREAKPOINTS) return fallback(0);
  const int firstLineIndent = resolveFirstLineIndent(!firstLineConsumed, renderer, fontId);
  const int8_t cs = blockStyle.characterSpacing;
  const auto canBreak = [&](size_t k) { return TokenBoundary::allowsBreak(continuesVec[k], noSpaceBeforeVec[k]); };
  const auto isRuby = [&](size_t i) {
    return i < rubyTexts.size() && ((wordStyles[i] & EpdFontFamily::RUBY_CONTINUE) || !rubyTexts[i].empty());
  };
  int grpW = 0;
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    const int prevW = i && !canBreak(i) ? grpW : 0;
    grpW = prevW + wordWidths[i];
    const int lim = effectiveWidth - (hyphenationEnabled && prevW && prevW < effectiveWidth && !isRuby(i) ? prevW : 0);
    while (wordWidths[i] > lim && lim > 0 &&
           ((hyphenationEnabled && hyphenateWordAtIndex(i, lim, renderer, fontId, wordWidths, false)) ||
            hyphenateWordAtIndex(i, lim, renderer, fontId, wordWidths, true)))
      grpW = wordWidths[i];
  }
  const size_t nWords = words.size();
  if (nWords >= MAX_DP_BREAKPOINTS) return fallback(0);
  const auto gapAt = [&](size_t w) -> int {
    if (!continuesVec[w] && noSpaceBeforeVec[w]) return cs;
    const uint32_t l = lastCodepoint(wordAt(w - 1)), f = firstCodepoint(wordAt(w));
    if (continuesVec[w]) return renderer.getKerning(fontId, l, f, getWordStyleAt(w - 1), cs);
    return scaleSpace(renderer.getSpaceAdvance(fontId, l, f, getWordStyleAt(w - 1)), wordSpacingPercent);
  };
  size_t M = 0, fitW = rubyTexts.empty() ? 0 : nWords + 1;
  for (int tot = 0; fitW < nWords && !(wordStyles[fitW] & TextBlock::DISCRETIONARY_HYPHEN_FLAG); ++fitW)
    if ((tot += wordWidths[fitW] + (fitW ? gapAt(fitW) : 0)) > pageWidth - firstLineIndent) break;
  if (fitW == nWords) return {nWords};
  const size_t bpCap = std::min<size_t>(MAX_DP_BREAKPOINTS, hyphenationEnabled ? nWords * 4 + 1 : nWords + 1);
  if (!(bps = makeUniqueNoThrow<BreakPoint[]>(bpCap))) return fallback(bpCap * sizeof(BreakPoint));
  const auto u16 = [](size_t x) { return static_cast<uint16_t>(x); };
  const bool just = blockStyle.alignment == CssTextAlign::Justify, useOpt = just && !blockStyle.isRtl && !hasRtlWord;
  std::string word;
  const uint8_t regH = optHang(useOpt, renderer, fontId, "-", EpdFontFamily::REGULAR, 0, false);
  for (size_t w = 0; w < nWords; ++w) {
    const auto wv = wordAt(w);
    const auto st = getWordStyleAt(w);
    const int wf = wordFontId(w, fontId);
    const uint8_t fb = wordFocusBoundary[w];
    bps[M++] = {u16(w), static_cast<uint16_t>(w ? gapAt(w) : 0), wordWidths[w], 0,
                BreakPoint::flg(false, optHang(useOpt, renderer, wf, wv, st, fb, true),
                                optHang(useOpt, renderer, wf, wv, st, fb, false))};
    if (!hyphenationEnabled || M + nWords - w >= bpCap || isRuby(w) || isRuby(w + 1) || (w && isRuby(w - 1))) continue;
    const uint8_t hc = hyphenClass(wv);
    if (!hc) continue;
    word.assign(wv.data(), wv.size());
    const bool canRegH = !(hc & 6) && wf == fontId && st == EpdFontFamily::REGULAR;
    for (const auto& [off, h] : collectWordBreaks(word, hc, false)) {
      if (M + nWords - w >= bpCap) break;
      if (!off || off >= wv.size() || off > MAX_HYPHEN_WORD_BYTES) continue;
      const auto pre = wv.substr(0, off), suf = wv.substr(off);
      const uint8_t pf = focusBoundaryBefore(fb, off), sf = focusBoundaryAfter(fb, off);
      const uint8_t rH = h && canRegH && !pf ? regH : optHang(useOpt, renderer, wf, pre, st, pf, false, h);
      bps[M++] = {u16(w), measureFocusWordWidth(renderer, wf, pre, st, pf, cs, wordSpacingPercent, h),
                  measureFocusWordWidth(renderer, wf, suf, st, sf, cs, wordSpacingPercent), static_cast<uint8_t>(off),
                  BreakPoint::flg(h, optHang(useOpt, renderer, wf, suf, st, sf, true), rH)};
    }
  }
  std::string().swap(word);
  bps[M] = {u16(nWords), 0, 0, 0, 0};
  auto dp = makeUniqueNoThrow<DpCell[]>(M + 1);
  if (!dp) return fallback((M + 1) * sizeof(DpCell));
  const int spW = std::max(1, scaleSpace(renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR), wordSpacingPercent));
  for (size_t u = M; u-- > 0;) {
    dp[u] = {{DP_INF, DP_INF}, {u16(M), u16(M)}};
    const size_t w = bps[u].word, sOff = bps[u].off;
    size_t sb = u, gaps = 0, spGaps = 0;
    if ((w || paragraphStartEmitted) && !sOff)
      while (bps[sb].word + 1 < nWords && continuesVec[bps[sb].word] && wordAt(bps[sb].word) == " ") ++sb;
    size_t sW = bps[sb].word, bIdx = sb, j = sW;
    int natW = sOff ? 0 : calculateRubyExtraStartOffset(sW, nWords, renderer, fontId), totalSpW = 0;
    const int ind = u ? 0 : firstLineIndent, lim0 = pageWidth - ind + (ind < 0 || natW ? 0 : (bps[sb].flags >> 1) & 3);
    const int maxL = lim0 + (useOpt ? 7 : 4);
    const auto tryEdge = [&](size_t v, int lim, int width, int shrink, bool isLast, bool hyph) {
      if (width - shrink > lim) return false;
      uint64_t rawDem = 100;
      if (isLast) {
        if (incLast && (u || firstLineConsumed || paragraphStartEmitted) && std::max(0, width) * 5 < lim0)
          rawDem += static_cast<uint32_t>(((lim0 / 5 - std::max(0, width)) * 4000) / std::max(1, lim0 / 5));
      } else {
        uint64_t t = 10 + (hyph ? 50 : 0);
        if (const int s = lim - width) {
          const int32_t c = std::min<int32_t>(s < 0 ? -s : s, 8192);
          const int64_t cap = std::max(1, s < 0 ? shrink : (!gaps ? spW / 2 : (spGaps ? (totalSpW + 1) / 2 : spW * 2)));
          int64_t b = s > 0 && !just ? (c * c * 4) / spW : (s > 0 && !gaps ? 5000 : 0);
          if (s < 0 || just) b += ((s < 0 ? 150LL : 100LL) * c * c * c) / (cap * cap * cap);
          t += static_cast<uint64_t>(b > 10000 ? std::min<int64_t>(30000, 10000 + (b - 10000) / 32) : b);
        }
        rawDem = t * t + (hyph && incLast && j + 1 == nWords ? 5000ULL : 0ULL);
      }
      const uint64_t dem = std::max<uint64_t>(1, rawDem >> 7);
      for (int p = 0; p < 2; ++p)
        if (const uint32_t tot = satDem(dem + (p && hyph ? 1953ULL : 0ULL) + dp[v].cost[hyph]);
            tot < dp[u].cost[p] || (tot == dp[u].cost[p] && tot < DP_SAT)) {
          dp[u].cost[p] = tot;
          dp[u].next[p] = u16(v);
        }
      return true;
    };
    for (; j < nWords; ++j) {
      const size_t first = bIdx;
      while (bps[bIdx].word == j) ++bIdx;
      const int gap = j > sW ? static_cast<int16_t>(bps[first].preW) : 0;
      const bool g1 = j > sW && !continuesVec[j], sp = g1 && !noSpaceBeforeVec[j];
      gaps += g1 + (j > sW + 1 && continuesVec[j - 1] && continuesVec[j] && wordAt(j - 1) == " ");
      spGaps += sp;
      totalSpW += sp ? gap : 0;
      const int preJ = natW + gap, shrink = maxLineShrinkPx(just, gaps, spGaps, totalSpW, spW);
      if (j > sW && preJ - shrink > maxL) break;
      natW = preJ + (j == w && sOff ? bps[u].sufW : wordWidths[j]);
      const bool last = j + 1 == nWords, skip = last && incLast && (dp[u].cost[0] < DP_INF || natW <= lim0);
      for (size_t v = j > w || !sOff ? first + 1 : bIdx; v < bIdx; ++v)
        if (!(bps[v].flags & 1) || !skip)
          tryEdge(v, lim0 + (gaps ? (bps[v].flags >> 3) & 3 : 0), preJ + bps[v].preW, shrink, false, bps[v].flags & 1);
      if (!last && !canBreak(j + 1)) {
        if (natW - shrink > maxL && j > sW) break;
        continue;
      }
      const int endE = calculateRubyExtraEndOffset(sW, j + 1, renderer, fontId);
      const int rH = gaps && !last && !endE ? (bps[first].flags >> 3) & 3 : 0;
      const bool dh = (wordStyles[j] & TextBlock::DISCRETIONARY_HYPHEN_FLAG) != 0;
      if ((!tryEdge(bIdx, lim0 + rH, natW + endE, last ? 0 : shrink, last, dh) && natW - shrink > maxL) || dh) break;
    }
    if (dp[u].cost[0] == DP_INF) {
      while (sb < M && (bps[sb].word <= sW || !canBreak(bps[sb].word))) ++sb;
      dp[u] = {{satDem(9500000ULL + dp[sb].cost[0]), satDem(9500000ULL + dp[sb].cost[0])}, {u16(sb), u16(sb)}};
    }
  }
  std::vector<size_t> lineBreakIndices;
  lineBreakIndices.reserve(std::min<size_t>(M, 64));
  for (size_t u = 0, shift = 0, st = 0, nxt = 0; u < M && (nxt = dp[u].next[st]) > u && nxt <= M; u = nxt) {
    const BreakPoint& bp = bps[nxt];
    const bool dh = (bp.flags & 1) != 0;
    size_t nb = bp.word + shift, prev = lineBreakIndices.empty() ? 0 : lineBreakIndices.back();
    if (!bp.off)
      st = bp.word && (wordStyles[nb - 1] & TextBlock::DISCRETIONARY_HYPHEN_FLAG);
    else if (nb >= prev && splitWordAtOffset(nb, bp.off, dh, bp.preW, renderer, fontId, wordWidths, bp.sufW)) {
      nb = bp.word + ++shift;
      st = dh;
    } else {
      st = 0;
      while (nb > prev && !canBreak(nb)) --nb;
      if (nb <= prev) nb = bp.word + shift + 1;
      while (nb > prev && nb < words.size() && !canBreak(nb)) ++nb;
    }
    if (lineBreakIndices.empty() || nb > prev) lineBreakIndices.push_back(nb);
  }
  if (lineBreakIndices.empty() || lineBreakIndices.back() < words.size()) lineBreakIndices.push_back(words.size());
  return lineBreakIndices;
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec,
                                                            std::vector<bool>& noSpaceBeforeVec,
                                                            const size_t maxLines) {
  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = !firstLineConsumed;

  while (currentIndex < wordWidths.size() && lineBreakIndices.size() < maxLines) {
    const size_t lineStart = currentIndex;
    size_t lastFittingBreak = lineStart;
    int lineWidth = calculateRubyExtraStartOffset(lineStart, wordWidths.size(), renderer, fontId);

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord && continuesVec[currentIndex]) {
        // Attached and breakable-attached boundaries both use kerning when kept on one line.
        spacing =
            renderer.getKerning(fontId, lastCodepoint(wordAt(currentIndex - 1)), firstCodepoint(wordAt(currentIndex)),
                                getWordStyleAt(currentIndex - 1), blockStyle.characterSpacing);
      } else if (!isFirstWord && noSpaceBeforeVec[currentIndex]) {
        spacing = blockStyle.characterSpacing;
      } else if (!isFirstWord) {
        spacing =
            scaleSpace(renderer.getSpaceAdvance(fontId, lastCodepoint(wordAt(currentIndex - 1)),
                                                firstCodepoint(wordAt(currentIndex)), getWordStyleAt(currentIndex - 1)),
                       wordSpacingPercent);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      const int extraEndOffset = calculateRubyExtraEndOffset(lineStart, currentIndex + 1, renderer, fontId);
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        if (lineWidth + extraEndOffset <= effectivePageWidth &&
            (currentIndex == wordWidths.size() ||
             TokenBoundary::allowsBreak(continuesVec[currentIndex], noSpaceBeforeVec[currentIndex]))) {
          lastFittingBreak = currentIndex;
        }
        if ((wordStyles[currentIndex - 1] & TextBlock::DISCRETIONARY_HYPHEN_FLAG) != 0) break;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (lineWidth + candidateWidth > effectivePageWidth && availableWidth > 0 &&
          (isFirstWord || (hyphenationEnabled && hyphenClass(wordAt(currentIndex)))) &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        ++currentIndex;
        lastFittingBreak = currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        ++currentIndex;
      }
      break;
    }

    if (lastFittingBreak > lineStart) currentIndex = lastFittingBreak;

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() &&
           !TokenBoundary::allowsBreak(continuesVec[currentIndex], noSpaceBeforeVec[currentIndex])) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  // Stable copy: Hyphenator and the prefix measurements below want std::string
  // semantics. Bounded by one word.
  const std::string word{wordAt(wordIndex)};  // brace-init: `word(` is an Arduino macro
  const auto style = getWordStyleAt(wordIndex);

  const uint8_t focusBoundary = wordFocusBoundary[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements). Focus emphasis is a byte
  // annotation, so the hyphenator sees the whole word and every legal break is reachable.
  // Hangul breaks come first so they win a tie against a hyphenated break at the same width.
  const auto breakInfos = collectWordBreaks(word, static_cast<uint8_t>(hyphenClass(word) | 1), allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (!offset || offset >= word.size() || offset > MAX_HYPHEN_WORD_BYTES) continue;

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureFocusWordWidth(renderer, wordFontId(wordIndex, fontId), word.substr(0, offset),
                                                  style, focusBoundaryBefore(focusBoundary, offset),
                                                  blockStyle.characterSpacing, wordSpacingPercent, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) return false;
  return splitWordAtOffset(wordIndex, chosenOffset, chosenNeedsHyphen, chosenWidth, renderer, fontId, wordWidths);
}

bool ParsedText::splitWordAtOffset(size_t wordIndex, size_t chosenOffset, bool chosenNeedsHyphen, int chosenWidth,
                                   const GfxRenderer& r, int fontId, std::vector<uint16_t>& wordWidths, int sufW) {
  if (wordIndex >= words.size() || !chosenOffset || chosenOffset >= wordAt(wordIndex).size()) return false;
  const std::string_view word = wordAt(wordIndex);
  const auto style = getWordStyleAt(wordIndex);
  const uint8_t focusBoundary = wordFocusBoundary[wordIndex];

  uint32_t prefixLength = 0;
  const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(word.data());
  const unsigned char* splitPtr = offsetPtr + chosenOffset;
  while (offsetPtr < splitPtr) {
    utf8NextCodepoint(&offsetPtr);
    prefixLength++;
  }
  const uint32_t remainderOffset = sourceOffsetAfter(visibleOffsetAt(wordIndex), prefixLength);

  // Split the word at the selected breakpoint. The prefix is materialized as a
  // fresh arena entry (with its visible hyphen, so it stays NUL-terminated);
  // the remainder aliases the original word's tail bytes and inherits the
  // original entry's release obligation via WordStore::suffix().
  // Stack buffer, not std::string: this runs mid-pagination, exactly when the
  // heap is under section-build pressure. The parser caps words at
  // MAX_WORD_SIZE (200) bytes; anything larger skips the split rather than
  // overflow (the word then breaks whole, as when no breakpoint fits).
  WordStore::StoredWord prefixStored;
  if (!appendSplitPrefix(wordStore, word, chosenOffset, chosenNeedsHyphen, prefixStored)) return false;
  const WordStore::StoredWord remainderStored = WordStore::suffix(words[wordIndex], chosenOffset);
  const uint8_t remDisc = wordStyles[wordIndex] & TextBlock::DISCRETIONARY_HYPHEN_FLAG;
  words[wordIndex] = prefixStored;
  const auto storedStyle = static_cast<EpdFontFamily::Style>(style & ~EpdFontFamily::SMALL_CAPS);
  wordStyles[wordIndex] =
      static_cast<EpdFontFamily::Style>(storedStyle | (chosenNeedsHyphen ? TextBlock::DISCRETIONARY_HYPHEN_FLAG : 0));

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainderStored);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, static_cast<EpdFontFamily::Style>(storedStyle | remDisc));
  insertVisibleOffset(wordIndex + 1, remainderOffset);
  // Emphasis follows the text across the split, so a break at or after the boundary leaves the
  // remainder fully regular.
  wordFocusBoundary.insert(wordFocusBoundary.begin() + wordIndex + 1, focusBoundaryAfter(focusBoundary, chosenOffset));
  wordLinkIds.insert(wordLinkIds.begin() + wordIndex + 1, wordLinkIds[wordIndex]);
  wordSizeSlots.insert(wordSizeSlots.begin() + wordIndex + 1, wordSizeSlots[wordIndex]);
  wordFocusBoundary[wordIndex] = focusBoundaryBefore(focusBoundary, chosenOffset);
  // Invariant: a boundary is always strictly inside its token, so an all-bold part carries BOLD in
  // its style with boundary 0 and nothing downstream special-cases boundary == size.
  // An inserted '-' counts as covered: measureFocusWordWidth() measured the
  // hyphen bold whenever the boundary spans the whole text before it, so the
  // rendered styling must match or justified spacing drifts by the bold/
  // regular hyphen advance delta.
  if (wordFocusBoundary[wordIndex] + (chosenNeedsHyphen ? 1u : 0u) >= words[wordIndex].len) {
    wordStyles[wordIndex] = static_cast<EpdFontFamily::Style>(wordStyles[wordIndex] | EpdFontFamily::BOLD);
    wordFocusBoundary[wordIndex] = 0;
  }
  if (wordIndex + 1 <= rubyTexts.size()) {
    rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, "");
  }

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
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, !chosenNeedsHyphen);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, !chosenNeedsHyphen);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth =
      sufW >= 0
          ? static_cast<uint16_t>(sufW)
          : measureFocusWordWidth(r, wordFontId(wordIndex, fontId), wordStore.view(remainderStored), style,
                                  wordFocusBoundary[wordIndex + 1], blockStyle.characterSpacing, wordSpacingPercent);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  // A no-break space carried across a soft wrap must not indent the next line.
  if (breakIndex > 0 || paragraphStartEmitted) {
    while (lastBreakAt + 1 < lineBreak && continuesVec[lastBreakAt] && wordAt(lastBreakAt) == " ") {
      ++lastBreakAt;
    }
  }
  const size_t lineWordCount = lineBreak - lastBreakAt;
  const uint32_t lineVisibleOffset = visibleOffsetAt(lastBreakAt);

  const int firstLineIndent = resolveFirstLineIndent(breakIndex == 0, renderer, fontId);

  std::vector<std::string> lineRubyTexts;
  if (!rubyTexts.empty() && lastBreakAt < rubyTexts.size()) {
    lineRubyTexts.resize(lineWordCount);
    const size_t copyCount = std::min(lineBreak, rubyTexts.size()) - lastBreakAt;
    std::copy(rubyTexts.begin() + lastBreakAt, rubyTexts.begin() + lastBreakAt + copyCount, lineRubyTexts.begin());
  }

  const int extraStartOffset = calculateRubyExtraStartOffset(lastBreakAt, lineBreak, renderer, fontId);
  const int extraEndOffset = calculateRubyExtraEndOffset(lastBreakAt, lineBreak, renderer, fontId);

  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  std::vector<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; ++i) {
    // Copy out of the arena: TextBlock construction and bidi reorder below
    // want owning strings. Bounded by one line; the arena bytes are released
    // by the consumed-prefix pass in layoutAndExtractLines.
    std::string word{wordAt(lastBreakAt + i)};  // brace-init: `word(` is an Arduino macro
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
    lineWords.push_back(std::move(word));
    // TextBlock keeps small caps with its word fonts (see attachWordFonts).
    lineWordStyles.push_back(
        static_cast<EpdFontFamily::Style>(getWordStyleAt(lastBreakAt + i) & ~EpdFontFamily::SMALL_CAPS));
  }

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0, spGaps = 0, spWTotal = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    if (wordIdx == 0) continue;
    const size_t boundaryIdx = lastBreakAt + wordIdx;
    const bool isStretchableSpace =
        lineWords[wordIdx] == " " && wordIdx + 1 < lineWordCount && continuesVec[boundaryIdx + 1];
    if (TokenBoundary::isJustifiableGap(continuesVec[boundaryIdx], isStretchableSpace)) {
      actualGapCount++;
    }
    if (continuesVec[boundaryIdx]) {
      totalNaturalGaps +=
          renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx - 1]), firstCodepoint(lineWords[wordIdx]),
                              lineWordStyles[wordIdx - 1], blockStyle.characterSpacing);
    } else if (noSpaceBeforeVec[boundaryIdx]) {
      totalNaturalGaps += blockStyle.characterSpacing;
    } else {
      const int prevGaps = totalNaturalGaps;
      totalNaturalGaps +=
          scaleSpace(renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                              firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]),
                     wordSpacingPercent);
      spGaps++;
      spWTotal += totalNaturalGaps - prevGaps;
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = lineBreak == words.size();

  // For RTL, implicit/default Left alignment becomes Right alignment.
  // Explicit text-align:left must remain left for CSS correctness.
  const CssTextAlign effectiveAlignment =
      (blockStyle.isRtl && !blockStyle.textAlignDefined && blockStyle.alignment == CssTextAlign::Left)
          ? CssTextAlign::Right
          : blockStyle.alignment;

  // For justified text, compute per-gap extra to distribute remaining space evenly.
  // extraEndOffset reserves space for any ruby group at the right edge of the line.
  const int spareSpace = effectivePageWidth - extraStartOffset - extraEndOffset - lineWordWidthSum - totalNaturalGaps;
  const int spW = std::max(1, scaleSpace(renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR), wordSpacingPercent));
  const bool just = effectiveAlignment == CssTextAlign::Justify && !isLastLine;
  const bool opt = effectiveAlignment == CssTextAlign::Justify && !blockStyle.isRtl && !hasRtlWord && lineWordCount;
  const auto oh = [&](size_t k, bool l) {
    return optHang(true, renderer, wordFontId(k, fontId), wordAt(k), getWordStyleAt(k), wordFocusBoundary[k], l);
  };
  const int lH = opt && firstLineIndent >= 0 && !extraStartOffset ? oh(lastBreakAt, true) : 0;
  const int rH = opt && just && actualGapCount && !extraEndOffset ? oh(lineBreak - 1, false) : 0;

  // BiDi processing: reorder words with UAX#9 in full-line context.
  visualOrderScratch.clear();
  visualOrderScratch.reserve(lineWordCount);
  // Skip expensive visual-order resolution for pure LTR paragraphs that have no RTL words.
  const bool shouldResolveVisualOrder = blockStyle.isRtl || hasRtlWord;
  const bool willReorder =
      shouldResolveVisualOrder && BidiUtils::computeVisualWordOrder(lineWords, blockStyle.isRtl, visualOrderScratch);

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  if (willReorder) {
    reorderedWordsScratch.clear();
    reorderedStylesScratch.clear();
    reorderedWidthsScratch.clear();
    reorderedContinuesScratch.clear();
    reorderedNoSpaceBeforeScratch.clear();
    reorderedFocusBoundaryScratch.clear();
    reorderedWordsScratch.reserve(visualOrderScratch.size());
    reorderedStylesScratch.reserve(visualOrderScratch.size());
    reorderedWidthsScratch.reserve(visualOrderScratch.size());
    reorderedContinuesScratch.reserve(visualOrderScratch.size());
    reorderedNoSpaceBeforeScratch.reserve(visualOrderScratch.size());
    reorderedFocusBoundaryScratch.reserve(visualOrderScratch.size());

    for (size_t i = 0; i < visualOrderScratch.size(); ++i) {
      const uint16_t src = visualOrderScratch[i];
      reorderedWordsScratch.push_back(std::move(lineWords[src]));
      reorderedStylesScratch.push_back(lineWordStyles[src]);
      reorderedWidthsScratch.push_back(wordWidths[lastBreakAt + src]);
      reorderedFocusBoundaryScratch.push_back(wordFocusBoundary[lastBreakAt + src]);

      // Continuation means "no break/gap between two adjacent logical tokens".
      // After visual reordering (common in RTL), an adjacent logical pair can appear
      // as either (prev -> curr) or (curr -> prev) in visual order; preserve both.
      bool continues = false;
      if (i > 0) {
        const size_t prevSrc = visualOrderScratch[i - 1];
        const size_t currSrc = src;
        const bool forwardAdjacent = currSrc == prevSrc + 1;
        const bool reverseAdjacent = prevSrc == currSrc + 1;

        if (forwardAdjacent && continuesVec[lastBreakAt + currSrc]) {
          continues = true;
        } else if (reverseAdjacent && continuesVec[lastBreakAt + prevSrc]) {
          continues = true;
        }
      }
      reorderedContinuesScratch.push_back(continues);
      reorderedNoSpaceBeforeScratch.push_back(!continues && noSpaceBeforeVec[lastBreakAt + src]);
    }

    int reorderedWordWidthSum = 0;
    size_t reorderedGapCount = 0;
    int reorderedNaturalGaps = 0, rSpGaps = 0, rSpW = 0;
    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      reorderedWordWidthSum += reorderedWidthsScratch[wordIdx];
      if (wordIdx > 0 && reorderedNoSpaceBeforeScratch[wordIdx]) {
        // Unicode break opportunity with no inserted Latin-style space. It is still
        // a stretchable gap for justified CJK/Korean text.
        reorderedGapCount++;
        reorderedNaturalGaps += blockStyle.characterSpacing;
      } else if (wordIdx > 0 && !reorderedContinuesScratch[wordIdx]) {
        const int prevGaps = reorderedNaturalGaps;
        reorderedGapCount++;
        reorderedNaturalGaps +=
            scaleSpace(renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                                firstCodepoint(reorderedWordsScratch[wordIdx]),
                                                reorderedStylesScratch[wordIdx - 1]),
                       wordSpacingPercent);
        rSpGaps++;
        rSpW += reorderedNaturalGaps - prevGaps;
      } else if (wordIdx > 0 && reorderedContinuesScratch[wordIdx]) {
        if (reorderedWordsScratch[wordIdx] == " " && wordIdx + 1 < reorderedWidthsScratch.size() &&
            reorderedContinuesScratch[wordIdx + 1]) {
          reorderedGapCount++;
        }
        reorderedNaturalGaps += renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                                    firstCodepoint(reorderedWordsScratch[wordIdx]),
                                                    reorderedStylesScratch[wordIdx - 1], blockStyle.characterSpacing);
      }
    }

    const int reorderedSpare =
        effectivePageWidth - extraStartOffset - extraEndOffset - reorderedWordWidthSum - reorderedNaturalGaps;
    auto reorderedDist = JustifyDistributor::make(just, reorderedSpare, reorderedGapCount, rSpGaps, rSpW, spW);
    const int contentWidth = reorderedWordWidthSum + reorderedNaturalGaps + reorderedDist.total();

    int xpos = 0;
    if (blockStyle.isRtl) {
      if (effectiveAlignment == CssTextAlign::Right || effectiveAlignment == CssTextAlign::Justify) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    } else {
      xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    }

    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      lineXPos.push_back(static_cast<int16_t>(xpos));
      xpos += reorderedWidthsScratch[wordIdx];

      const bool nextIsContinuation =
          wordIdx + 1 < reorderedWidthsScratch.size() && reorderedContinuesScratch[wordIdx + 1];
      if (nextIsContinuation) {
        int advance = renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                          firstCodepoint(reorderedWordsScratch[wordIdx + 1]),
                                          reorderedStylesScratch[wordIdx], blockStyle.characterSpacing);
        // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
        // no-break space must not receive justifyExtra, or the line over-stretches by one
        // gap and the last word is pushed past the right margin (issue #2185).
        if (wordIdx > 0 && reorderedWordsScratch[wordIdx] == " " && reorderedContinuesScratch[wordIdx] &&
            effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          advance += reorderedDist.next();
        }
        xpos += advance;
      } else if (wordIdx + 1 < reorderedWidthsScratch.size()) {
        const bool nextNoSpace = reorderedNoSpaceBeforeScratch[wordIdx + 1];
        int gap = nextNoSpace
                      ? blockStyle.characterSpacing
                      : scaleSpace(renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                                            firstCodepoint(reorderedWordsScratch[wordIdx + 1]),
                                                            reorderedStylesScratch[wordIdx]),
                                   wordSpacingPercent);
        if (effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          gap += reorderedDist.next();
        }
        xpos += gap;
      }
    }

    lineWords.swap(reorderedWordsScratch);
    lineWordStyles.swap(reorderedStylesScratch);
  } else {
    auto justifyDist = JustifyDistributor::make(just, spareSpace + lH + rH, actualGapCount, spGaps, spWTotal, spW);
    // Standard LTR/RTL positioning loop when no visual reordering is needed
    if (blockStyle.isRtl) {
      // RTL: position words from right to left
      int xpos = effectivePageWidth;
      if (effectiveAlignment == CssTextAlign::Left) {
        // Explicit left alignment in RTL context
        xpos = lineWordWidthSum + totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth + lineWordWidthSum + totalNaturalGaps) / 2;
      }
      // For Right and Justify, start from right edge (xpos = effectivePageWidth)

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        xpos -= wordWidths[lastBreakAt + wordIdx];
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          // Cross-boundary kerning for continuation words
          int advance =
              renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]), firstCodepoint(lineWords[wordIdx + 1]),
                                  lineWordStyles[wordIdx], blockStyle.characterSpacing);
          // wordIdx > 0: see the LTR branch — a leading no-break space is not a justifiable gap.
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyDist.next();
          }
          xpos -= advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace ? blockStyle.characterSpacing
                              : scaleSpace(renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                                    firstCodepoint(lineWords[wordIdx + 1]),
                                                                    lineWordStyles[wordIdx]),
                                           wordSpacingPercent);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyDist.next();
          }
          xpos -= gap;
        }
      }
    } else {
      // LTR: position words from left to right
      int xpos = firstLineIndent + extraStartOffset - lH;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
      }

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          int advance = wordWidths[lastBreakAt + wordIdx];
          advance +=
              renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]), firstCodepoint(lineWords[wordIdx + 1]),
                                  lineWordStyles[wordIdx], blockStyle.characterSpacing);
          // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
          // no-break space must not receive justifyExtra, or the line over-stretches by one
          // gap and the last word is pushed past the right margin (issue #2185).
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyDist.next();
          }
          xpos += advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace ? blockStyle.characterSpacing
                              : scaleSpace(renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                                    firstCodepoint(lineWords[wordIdx + 1]),
                                                                    lineWordStyles[wordIdx]),
                                           wordSpacingPercent);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyDist.next();
          }
          xpos += wordWidths[lastBreakAt + wordIdx] + gap;
        }
      }
    }
  }

  const auto focusBoundaryAt = [&](const size_t idx) {
    return willReorder ? reorderedFocusBoundaryScratch[idx] : wordFocusBoundary[lastBreakAt + idx];
  };

  std::vector<TextBlock::LinkSpan> lineLinks;
  std::vector<uint8_t> lineLinkIdsSeen;
  for (size_t i = 0; i < lineWordCount; i++) {
    const uint8_t linkId = wordLinkIds[lastBreakAt + (willReorder ? visualOrderScratch[i] : i)];
    if (linkId == 0 || linkId > linkTargets.size()) continue;

    size_t spanIndex = 0;
    while (spanIndex < lineLinkIdsSeen.size() && lineLinkIdsSeen[spanIndex] != linkId) spanIndex++;
    int width = willReorder ? reorderedWidthsScratch[i] : wordWidths[lastBreakAt + i];
    const int right = lineXPos[i] + width;
    const int topLift =
        (lineWordStyles[i] & EpdFontFamily::SUP) != 0 ? renderer.getFontAscenderSize(fontId) * 2 / 5 : 0;

    if (spanIndex == lineLinkIdsSeen.size()) {
      lineLinks.emplace_back();
      auto& span = lineLinks.back();
      strncpy(span.href, linkTargets[linkId - 1].c_str(), sizeof(span.href) - 1);
      span.href[sizeof(span.href) - 1] = '\0';
      span.x = lineXPos[i];
      span.width = static_cast<int16_t>(width);
      span.topLift = static_cast<int16_t>(topLift);
      lineLinkIdsSeen.push_back(linkId);
    } else {
      auto& span = lineLinks[spanIndex];
      const int left = std::min<int>(span.x, lineXPos[i]);
      const int mergedRight = std::max<int>(span.x + span.width, right);
      span.x = static_cast<int16_t>(left);
      span.width = static_cast<int16_t>(mergedRight - left);
      span.topLift = std::max<int16_t>(span.topLift, static_cast<int16_t>(topLift));
    }
  }

  // Complex-script words keep the form they were measured in (shaped, via
  // this layout's shaping memo) in the page cache, so page turns draw exactly
  // that without shaping. Empty when the line has none. A form that does not
  // fill the word's measured width (this pass shaped differently, e.g. the
  // memo was full and memory ran short; or ruby widened the slot) is dropped,
  // and the word resolves when drawn.
  std::vector<std::string> lineDisplay;
  for (size_t i = 0; hasComplexWord && i < lineWordCount; i++) {
    if (!ComplexShaper::containsComplexScript(lineWords[i].c_str())) continue;
    if (lineDisplay.empty()) lineDisplay.resize(lineWordCount);
    std::string& form = lineDisplay[i];
    // The font and style layout measured the word in.
    const size_t logical = lastBreakAt + (willReorder ? visualOrderScratch[i] : i);
    const int wordFont = wordFontId(logical, fontId);
    const EpdFontFamily::Style wordStyle = getWordStyleAt(logical);
    if (!renderer.resolveForDisplay(wordFont, lineWords[i].c_str(), wordStyle, form)) continue;
    const int measured = willReorder ? reorderedWidthsScratch[i] : wordWidths[lastBreakAt + i];
    const int formWidth = renderer.getTextAdvanceX(
        wordFont, GfxRenderer::LaidOutText{lineWords[i].c_str(), form.c_str()}, wordStyle, blockStyle.characterSpacing);
    if (formWidth != measured) {
      LOG_DBG("PTX", "Display form measures %d px, layout %d px: resolved when drawn", formWidth, measured);
      form.clear();
    }
  }

  // Lines mixing sizes or using small caps carry per-word fonts; slots resolving to the block
  // font collapse to 0.
  const auto attachWordFonts = [&](TextBlock& block) {
    int32_t fonts[TextBlock::MAX_WORD_FONTS] = {};
    uint8_t slots[128];
    if (lineWordCount > sizeof(slots)) return true;  // pathological line: keep the block font
    bool mixed = false;
    for (size_t i = 0; i < lineWordCount; ++i) {
      const size_t logical = lastBreakAt + (willReorder ? visualOrderScratch[i] : i);
      const int wordFont = wordFontId(logical, fontId);
      uint8_t slot = 0;
      if (wordFont != fontId) {
        for (slot = 1; slot <= TextBlock::MAX_WORD_FONTS; ++slot) {
          if (fonts[slot - 1] == wordFont || fonts[slot - 1] == 0) break;
        }
        if (slot > TextBlock::MAX_WORD_FONTS) slot = 0;
        if (slot > 0) fonts[slot - 1] = wordFont;
      }
      slots[i] = static_cast<uint8_t>(slot | (wordSizeSlots[logical] & TextBlock::SMALL_CAPS_SLOT_FLAG));
      mixed = mixed || slots[i] != 0;
    }
    return !mixed || block.setWordFonts(slots, fonts);
  };

  // Fast path: no word on this line carries focus emphasis, so pass empty boundary/suffixX
  // vectors. TextBlock pays zero per-word RAM cost for these annotations when they are empty.
  bool lineHasFocusSplit = false;
  for (size_t i = 0; i < lineWordCount; i++) {
    if (focusBoundaryAt(i) != 0) {
      lineHasFocusSplit = true;
      break;
    }
  }

  // Preserve source coordinates through visual reordering and discretionary hyphens.
  std::vector<TextBlock::SourceRange> sourceRanges;
  sourceRanges.reserve(lineWordCount);
  uint16_t paragraphStartWord = UINT16_MAX;
  for (size_t i = 0; i < lineWordCount; ++i) {
    const size_t logical = lastBreakAt + (willReorder ? visualOrderScratch[i] : i);
    if (!paragraphStartEmitted && logical == 0) paragraphStartWord = static_cast<uint16_t>(i);
    const uint32_t start = visibleOffsetAt(logical);
    const bool discretionaryHyphen = (wordStyles[logical] & TextBlock::DISCRETIONARY_HYPHEN_FLAG) != 0;
    uint32_t end = sourceOffsetAfter(start, countCodepoints(wordAt(logical)) - discretionaryHyphen);
    if (discretionaryHyphen) {
      lineWordStyles[i] = static_cast<EpdFontFamily::Style>(lineWordStyles[i] | TextBlock::DISCRETIONARY_HYPHEN_FLAG);
    }
    if (logical + 1 < words.size() && visibleOffsetAt(logical + 1) > start) {
      end = std::min(end, visibleOffsetAt(logical + 1));
    }
    sourceRanges.push_back({start, end});
  }

  if (!lineHasFocusSplit) {
    // TextBlock flattens the vectors into its arena; they stay owned here and die at return.
    auto block = makeUniqueNoThrow<TextBlock>(lineWords, lineXPos, lineWordStyles, std::vector<uint8_t>{},
                                              std::vector<uint16_t>{}, blockStyle, std::move(lineRubyTexts),
                                              std::move(lineLinks), sourceRanges, paragraphStartWord, lineDisplay);
    if (!block || !block->valid() || !attachWordFonts(*block)) {
      LOG_ERR("PTX", "Dropping line: TextBlock or arena allocation failed");
      // Latch through the same flag as addWord() OOM: the caller releases the
      // consumed words right after this returns, so without it the section
      // would commit with this line silently missing.
      droppedWords = true;
      return;
    }
    processLine(std::move(block), lineVisibleOffset);
    return;
  }

  // Each word is one TextBlock entry carrying its own boundary and an offset
  // to the second visual run: the bold prefix in RTL, the regular suffix in LTR.
  std::vector<uint8_t> outBoundaries;
  std::vector<uint16_t> outSplitX;
  outBoundaries.reserve(lineWordCount);
  outSplitX.reserve(lineWordCount);
  for (size_t i = 0; i < lineWordCount; i++) {
    const uint8_t boundary = focusBoundaryAt(i);
    outBoundaries.push_back(boundary);
    const int wordFont = wordFontId(lastBreakAt + (willReorder ? visualOrderScratch[i] : i), fontId);
    const auto style = getWordStyleAt(lastBreakAt + (willReorder ? visualOrderScratch[i] : i));
    const bool rtl = BidiUtils::startsWithRtl(lineWords[i].c_str());
    outSplitX.push_back(boundary == 0 ? 0
                        : rtl         ? measureFocusRtlSplitAdvance(renderer, wordFont, lineWords[i], style, boundary,
                                                                    blockStyle.characterSpacing)
                                      : measureFocusPrefixAdvance(renderer, wordFont, lineWords[i], style, boundary,
                                                                  blockStyle.characterSpacing));
  }

  auto block = makeUniqueNoThrow<TextBlock>(lineWords, lineXPos, lineWordStyles, outBoundaries, outSplitX, blockStyle,
                                            std::move(lineRubyTexts), std::move(lineLinks), sourceRanges,
                                            paragraphStartWord, lineDisplay);
  if (!block || !block->valid() || !attachWordFonts(*block)) {
    LOG_ERR("PTX", "Dropping line: TextBlock or arena allocation failed");
    droppedWords = true;  // see the non-focus branch above
    return;
  }
  processLine(std::move(block), lineVisibleOffset);
}

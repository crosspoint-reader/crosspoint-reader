#include "TextBlock.h"

#include <BuildArena.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

bool TextBlock::guideDotsEnabled = false;

namespace {
// Style and continuation share one arena byte: bits 0-6 are the EpdFontFamily::Style, bit 7
// says the word is glued to its predecessor. See TextBlock::wordContinues.
uint8_t packStyle(const EpdFontFamily::Style style, const bool continues) {
  return static_cast<uint8_t>((static_cast<uint8_t>(style) & ~TextBlock::WORD_CONTINUES_BIT) |
                              (continues ? TextBlock::WORD_CONTINUES_BIT : 0));
}
}  // namespace

TextBlock::ArenaOffsets TextBlock::arenaOffsets(const uint16_t wordCount, const bool hasSizes,
                                                const uint8_t fontCount) {
  // Layout documented in TextBlock.h: the word fonts (4-byte fields) first, then the 16-bit
  // arrays (textOff, xpos), then the 8-bit arrays (styles, optional sizes and font slots), then
  // the text blob. Without word fonts textOff sits at 0.
  const size_t wc = wordCount;
  ArenaOffsets o{};
  o.textOff = fontCount * sizeof(WordFont);
  o.xpos = o.textOff + wc * sizeof(uint16_t);
  o.styles = o.xpos + wc * sizeof(int16_t);
  o.sizes = o.styles + wc * sizeof(uint8_t);  // only meaningful when hasSizes
  o.fontSlots = o.sizes + (hasSizes ? wc * sizeof(uint8_t) : 0);
  o.text = o.fontSlots + (fontCount != 0 ? wc * sizeof(uint8_t) : 0);
  return o;
}

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasSizes, const uint8_t fontCount,
                            const uint16_t textBytes) {
  return arenaOffsets(wordCount, hasSizes, fontCount).text + textBytes;
}

bool TextBlock::allocArena(const size_t size, BuildArena* scratch) {
  if (scratch != nullptr) {
    // The two 16-bit arrays come first, so the base needs 2-byte alignment; 4 keeps the
    // cursor tidy for whatever follows.
    arena = static_cast<uint8_t*>(scratch->alloc(size, 4));
    if (arena != nullptr) {
      arenaOwned_ = false;
      return true;
    }
    // Region full (a page beyond what it was sized for): fall through to the heap for this
    // line alone. A mixed page is fine -- ownership is per block.
  }
  arena = new (std::nothrow) uint8_t[size];
  arenaOwned_ = arena != nullptr;
  return arena != nullptr;
}

void TextBlock::bindArenaPointers() {
  const uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, wordFontCount);
  textOffArr = reinterpret_cast<const uint16_t*>(base + o.textOff);
  xposArr = reinterpret_cast<const int16_t*>(base + o.xpos);
  stylesArr = base + o.styles;
  sizesArr = sizesPresent ? base + o.sizes : nullptr;
  textArr = reinterpret_cast<const char*>(base + o.text);
}

TextBlock::TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle,
                     std::vector<uint8_t> word_sizes, const std::vector<bool>& word_continues)
    // Narrow the parser's full BlockStyle to the render-only slice: the spacing
    // fields have already been consumed into word_xpos / the line's y by layout.
    : renderStyle{blockStyle.fontSizeMultiplier, blockStyle.headingFontId, blockStyle.alignment} {
  // Normalize the all-100% case to no sizes so uniform lines (the common case)
  // keep the zero-cost fast paths in render/serialize/line-height.
  if (std::all_of(word_sizes.begin(), word_sizes.end(), [](const uint8_t s) { return s == 100; })) {
    word_sizes.clear();
  }
  const bool hasSizes = !word_sizes.empty();

  if (words.size() != word_xpos.size() || words.size() != word_styles.size() || words.size() > 10000 ||
      (hasSizes && words.size() != word_sizes.size())) {
    LOG_ERR("TXB", "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, sizes=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(word_xpos.size()),
            static_cast<uint32_t>(word_styles.size()), static_cast<uint32_t>(word_sizes.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  sizesPresent = hasSizes;
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word. A line is at most a physical row
  // of the page, so uint16_t offsets are ample; reject anything larger.
  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    sizesPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, sizesPresent, 0, textBytes);
  if (!allocArena(size, nullptr)) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    sizesPresent = false;
    isValid = false;
    return;
  }

  // Pass 2: fill through mutable pointers derived from the same layout offsets
  // that bindArenaPointers() uses for the const views (bound below for reads).
  uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, 0);
  auto* textOff = reinterpret_cast<uint16_t*>(base);
  auto* xpos = reinterpret_cast<int16_t*>(base + o.xpos);
  uint8_t* styles = base + o.styles;
  char* text = reinterpret_cast<char*>(base + o.text);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = word_xpos[i];
    styles[i] = packStyle(word_styles[i], i < word_continues.size() && word_continues[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (sizesPresent) {
    uint8_t* sizes = base + o.sizes;
    for (uint16_t i = 0; i < numWords; i++) sizes[i] = word_sizes[i];
  }
  bindArenaPointers();
}

TextBlock::TextBlock(const WordRange& range, const std::vector<int16_t>& word_xpos, const BlockStyle& blockStyle,
                     BuildArena* scratch)
    : renderStyle{blockStyle.fontSizeMultiplier, blockStyle.headingFontId, blockStyle.alignment} {
  if (range.words == nullptr || range.styles == nullptr) {
    LOG_ERR("TXB", "Construction failed: null word range");
    isValid = false;
    return;
  }
  const std::vector<std::string>& words = *range.words;
  const std::vector<EpdFontFamily::Style>& styleSrc = *range.styles;
  const size_t first = range.first;
  const size_t count = range.count;

  // Every source array must actually span [first, first + count); xpos is per line so it is
  // indexed from 0 and only needs `count` entries.
  const bool hasSizeSrc = range.sizes != nullptr && !range.sizes->empty();
  // Optional, and only used when it actually spans the range -- a caller that does not track
  // continuation (or hands over a short vector) just produces space-separated words.
  const std::vector<TokenBoundary::Boundary>* boundariesSrc =
      (range.boundaries != nullptr && first + count <= range.boundaries->size()) ? range.boundaries : nullptr;
  if (count > 10000 || first + count > words.size() || first + count > styleSrc.size() || word_xpos.size() != count ||
      (hasSizeSrc && first + count > range.sizes->size())) {
    LOG_ERR("TXB", "Construction failed: range out of bounds (first=%u, count=%u, words=%u, xpos=%u)",
            static_cast<uint32_t>(first), static_cast<uint32_t>(count), static_cast<uint32_t>(words.size()),
            static_cast<uint32_t>(word_xpos.size()));
    isValid = false;
    return;
  }

  // Normalize the all-100% case to no sizes, matching the vector constructor so both paths
  // produce byte-identical arenas (and therefore identical section-cache bytes).
  bool anyNon100 = false;
  if (hasSizeSrc) {
    const std::vector<WordSize>& sizeSrc = *range.sizes;
    for (size_t i = 0; i < count && !anyNon100; i++) anyNon100 = sizeSrc[first + i].pct != 100;
  }
  sizesPresent = anyNon100;

  numWords = static_cast<uint16_t>(count);
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // The line's word fonts: the fonts of the range's table its words use, in the order they first
  // appear; a line whose words are all in the block's font has none, and its arena is as before.
  uint8_t lineSlot[kMaxWordFonts] = {};  // the range's slot -> the line's, 1-based; 0: the block's font
  uint8_t tableSlot[kMaxWordFonts];      // the line's slot -> the range's
  const bool hasFonts = hasSizeSrc && range.fontTable != nullptr;
  for (size_t i = 0; hasFonts && i < count; i++) {
    const uint8_t slot = (*range.sizes)[first + i].fontSlot % kMaxWordFonts;
    if (range.fontTable[slot].fontId == 0 || lineSlot[slot] != 0) continue;
    tableSlot[wordFontCount] = slot;
    lineSlot[slot] = ++wordFontCount;
  }

  // Pass 1: total text size, one NUL per word, soft hyphens excluded — they are stripped
  // during the copy below, so reserving their bytes would leave a gap between words.
  size_t totalText = 0;
  for (size_t i = 0; i < count; i++) {
    const std::string& w = words[first + i];
    size_t stripped = 0;
    for (size_t pos = w.find(SOFT_HYPHEN_UTF8); pos != std::string::npos;
         pos = w.find(SOFT_HYPHEN_UTF8, pos + SOFT_HYPHEN_BYTES)) {
      stripped += SOFT_HYPHEN_BYTES;
    }
    totalText += w.size() - stripped + 1;
  }
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    sizesPresent = false;
    wordFontCount = 0;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, sizesPresent, wordFontCount, textBytes);
  if (!allocArena(size, scratch)) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    sizesPresent = false;
    wordFontCount = 0;
    isValid = false;
    return;
  }

  // Pass 2: fill straight from the caller's arrays — no intermediate per-line vectors.
  uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, wordFontCount);
  auto* textOff = reinterpret_cast<uint16_t*>(base + o.textOff);
  auto* xpos = reinterpret_cast<int16_t*>(base + o.xpos);
  uint8_t* styles = base + o.styles;
  char* text = reinterpret_cast<char*>(base + o.text);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = word_xpos[i];
    styles[i] = packStyle(styleSrc[first + i], boundariesSrc != nullptr && (*boundariesSrc)[first + i].continues);
    // Copy the word one soft-hyphen-free run at a time.
    const std::string& w = words[first + i];
    size_t start = 0;
    for (size_t pos = w.find(SOFT_HYPHEN_UTF8); pos != std::string::npos; pos = w.find(SOFT_HYPHEN_UTF8, start)) {
      const size_t run = pos - start;
      memcpy(text + off, w.data() + start, run);
      off += static_cast<uint16_t>(run);
      start = pos + SOFT_HYPHEN_BYTES;
    }
    const size_t tail = w.size() - start;
    memcpy(text + off, w.data() + start, tail);
    off += static_cast<uint16_t>(tail);
    text[off++] = '\0';
  }
  if (sizesPresent) {
    uint8_t* sizes = base + o.sizes;
    const std::vector<WordSize>& sizeSrc = *range.sizes;
    for (uint16_t i = 0; i < numWords; i++) sizes[i] = sizeSrc[first + i].pct;
  }
  if (wordFontCount != 0) {
    auto* fonts = reinterpret_cast<WordFont*>(base);
    for (uint8_t f = 0; f < wordFontCount; f++) fonts[f] = range.fontTable[tableSlot[f]];
    uint8_t* slots = base + o.fontSlots;
    for (uint16_t i = 0; i < numWords; i++) slots[i] = lineSlot[(*range.sizes)[first + i].fontSlot % kMaxWordFonts];
  }
  bindArenaPointers();
}

uint8_t TextBlock::maxSizePct() const {
  if (!sizesPresent) {
    return 100;
  }
  uint8_t m = 0;
  for (uint16_t i = 0; i < numWords; i++) m = std::max(m, sizesArr[i]);
  return m;
}

// Mirrors the per-word geometry in render() below. Kept adjacent to it on
// purpose: the two must move together.
TextBlock::WordBox TextBlock::wordBox(const GfxRenderer& renderer, const uint16_t i, const int fontId, const int x,
                                      const int y) const {
  WordBox box;
  if (!isValid || i >= numWords) return box;

  const int effFontId = renderStyle.headingFontId != 0 ? renderStyle.headingFontId : fontId;
  const float blockScale = renderStyle.fontSizeMultiplier;
  const int blockAscender = (blockScale == 1.0f) ? renderer.getFontAscenderSize(effFontId)
                                                 : renderer.getFontAscenderSizeScaled(effFontId, blockScale);
  int lineAscender = blockAscender;
  if (sizesPresent) {
    lineAscender = renderer.getFontAscenderSizeScaled(effFontId, blockScale * (maxSizePct() / 100.0f));
  }

  const char* word = wordText(i);
  const EpdFontFamily::Style style = wordStyle(i);
  const int font = wordFontId(i, effFontId);
  const float scale = wordScale(i);
  const int ascender =
      (font == effFontId && scale == blockScale) ? blockAscender : renderer.getFontAscenderSizeScaled(font, scale);

  int wordY = y + (lineAscender - ascender);
  if ((style & EpdFontFamily::SUP) != 0) {
    wordY -= blockAscender * 2 / 5;
  } else if ((style & EpdFontFamily::SUB) != 0) {
    wordY += blockAscender / 4;
  }

  box.fontId = font;
  box.style = style;
  box.scale = scale;
  box.x = static_cast<int16_t>(xposArr[i] + x);
  box.y = static_cast<int16_t>(wordY);
  // Scaled measurement, not getTextAdvanceX: a book with publisher font sizes
  // gives words a per-word scale, and an unscaled width drifts wider or
  // narrower than the glyphs actually drawn.
  box.width = static_cast<int16_t>((scale == 1.0f) ? renderer.getTextWidth(font, word, style)
                                                   : renderer.getTextWidthScaled(font, word, style, scale));
  box.height =
      static_cast<int16_t>((scale == 1.0f) ? renderer.getLineHeight(font) : renderer.getLineHeightScaled(font, scale));
  return box;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  // Heading blocks may render with a taller real font (headingFontId) instead of scaling the
  // body font. In that case fontSizeMultiplier is a small residual (usually 1.0). Resolve the
  // effective (fontId, scale) pair once and use it for every measure/draw below.
  const int effFontId = renderStyle.headingFontId != 0 ? renderStyle.headingFontId : fontId;
  const float blockScale = renderStyle.fontSizeMultiplier;
  const int blockAscender = (blockScale == 1.0f) ? renderer.getFontAscenderSize(effFontId)
                                                 : renderer.getFontAscenderSizeScaled(effFontId, blockScale);
  // Mixed-size lines are baseline-aligned: every word's baseline sits at y + lineAscender,
  // where lineAscender is the tallest word's ascender. Uniform lines (no sizes) keep
  // lineAscender == blockAscender, i.e. the historical single-scale layout.
  int lineAscender = blockAscender;
  if (sizesPresent) {
    lineAscender = renderer.getFontAscenderSizeScaled(effFontId, blockScale * (maxSizePct() / 100.0f));
  }
  // Guide dots (see setGuideDots): one dot centered in each inter-word gap.
  // Sized and vertically anchored off the block's base metrics, not per-word
  // scales, so the dot row stays level across inline size changes.
  const bool guideDots = guideDotsEnabled && !scanning;
  const int dotSize = std::max(2, blockAscender / 8);
  int prevWordEndX = 0;  // right edge of the previous word; valid once i > 0
  // Cache per-word ascender calculations (typically 2-3 unique scales per block).
  // Avoids the per-word function call overhead on ESP32-C3 for typical books where
  // per-word font sizing is rare or uniform. A word in a font of its own (WordFont)
  // has its font's ascender.
  struct CachedAscender {
    int fontId;
    float scale;
    int ascender;
  };
  std::vector<CachedAscender> ascenderCache;
  auto getOrCacheAscender = [&](const int font, const float s) -> int {
    if (font == effFontId && s == blockScale) return blockAscender;
    for (const CachedAscender& cached : ascenderCache) {
      if (cached.fontId == font && cached.scale == s) return cached.ascender;
    }
    const int ascender = renderer.getFontAscenderSizeScaled(font, s);
    ascenderCache.push_back({font, s, ascender});
    return ascender;
  };
  for (uint16_t i = 0; i < numWords; i++) {
    const char* word = wordText(i);
    const int wordX = xposArr[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const int font = wordFontId(i, effFontId);
    const float scale = wordScale(i);
    const int ascender = getOrCacheAscender(font, scale);
    // Baseline alignment: shift smaller words down so all baselines meet at y + lineAscender. A word in
    // another family meets it too: the line keeps its block font's baseline, whatever the word's ascender.
    int wordY = y + (lineAscender - ascender);
    // SUP/SUB shift the baseline only — the glyph shrink comes from the word's own size
    // percentage (parser default 50%, or the publisher's CSS). Offsets are anchored to the
    // BLOCK's full-size ascender, not the shrunken word's, so the raised/lowered positions
    // match the surrounding full-size text:
    //   SUP: raise by 40% of the block ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of the block ascender — descends below baseline without clashing
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= blockAscender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += blockAscender / 4;
    }
    if (scale == 1.0f) {
      renderer.drawText(font, wordX, wordY, word, true, currentStyle);
    } else {
      renderer.drawTextScaled(font, wordX, wordY, word, true, currentStyle, scale);
    }

    const bool hasDecoration =
        !scanning && (currentStyle & (EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH)) != 0;
    int lineWidth = 0;
    if (guideDots || hasDecoration) {
      lineWidth = (scale == 1.0f) ? renderer.getTextWidth(font, word, currentStyle)
                                  : renderer.getTextWidthScaled(font, word, currentStyle, scale);
    }

    if (guideDots) {
      if (i > 0) {
        const int gap = wordX - prevWordEndX;
        // Skip cramped gaps (zero-width joins of adjacent styled runs): the dot
        // needs at least a pixel of clearance on each side to read as a dot.
        if (gap >= dotSize + 2) {
          const int dotX = prevWordEndX + (gap - dotSize) / 2;
          // A third of the ascender above the shared baseline -- roughly mid
          // x-height, like a typographic middle dot.
          const int dotY = y + lineAscender - blockAscender / 3 - dotSize / 2;
          renderer.fillRect(dotX, dotY, dotSize, dotSize, true);
        }
      }
      prevWordEndX = wordX + lineWidth;
    }

    if (hasDecoration) {
      if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
        const int underlineY = y + lineAscender + 3;
        renderer.drawLine(wordX, underlineY, wordX + lineWidth, underlineY, 2, true);
      }

      if ((currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
        const int strikeY = wordY + ascender / 2 + 4;
        renderer.drawLine(wordX, strikeY, wordX + lineWidth, strikeY, 2, true);
      }
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  // Word data: scalars, then the arena verbatim -- its in-memory layout is
  // exactly the on-disk layout (see TextBlock.h), so one write covers all
  // per-word arrays and the text blob. The flags byte holds the sizes in bit 0
  // and the word-font count in bits 4-7: a line without word fonts writes the
  // 0 or 1 it always did.
  serialization::writePod(file, numWords);
  serialization::writePod(file, static_cast<uint8_t>((sizesPresent ? 1 : 0) | wordFontCount << 4));
  serialization::writePod(file, textBytes);
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, sizesPresent, wordFontCount, textBytes);
    if (file.write(arena, size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  // Style: only the render slice. The spacing fields (margins, padding, indent
  // and their "defined" flags) used to be written here, but layout consumes them
  // into the word xpos / line y before a TextBlock exists, and nothing ever read
  // them back -- so they were 19 dead bytes per line on disk as well as in RAM.
  // Dropping them is a format change; SECTION_FILE_VERSION is bumped so stale
  // caches are rejected and rebuilt.
  serialization::writePod(file, renderStyle.alignment);
  serialization::writePod(file, renderStyle.fontSizeMultiplier);
  serialization::writePod(file, renderStyle.headingFontId);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file, BuildArena* scratch) {
  uint16_t wc = 0;
  uint8_t flags = 0;
  uint16_t textBytes = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&wc), sizeof(wc)) != static_cast<int>(sizeof(wc)) ||
      file.read(&flags, sizeof(flags)) != static_cast<int>(sizeof(flags)) ||
      file.read(reinterpret_cast<uint8_t*>(&textBytes), sizeof(textBytes)) != static_cast<int>(sizeof(textBytes))) {
    LOG_ERR("TXB", "Deserialization failed: incomplete arena header");
    return nullptr;
  }

  // Sanity checks: cap the arena allocation and reject impossible geometry
  // (every word carries at least its NUL terminator).
  const uint8_t fontCount = flags >> 4;
  if (wc > 10000 || (flags & 0x0E) != 0 || fontCount > kMaxWordFonts || (fontCount != 0 && wc == 0)) {
    LOG_ERR("TXB", "Deserialization failed: word count %u or flags 0x%02x out of range", wc, flags);
    return nullptr;
  }
  if ((wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  block->sizesPresent = (flags & 1) != 0;
  block->wordFontCount = fontCount;

  if (wc > 0) {
    const size_t size = arenaSize(wc, block->sizesPresent, fontCount, textBytes);
    if (!block->allocArena(size, scratch)) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
      return nullptr;
    }
    if (file.read(block->arena, size) != static_cast<int>(size)) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
      return nullptr;
    }
    block->bindArenaPointers();

    // Validate offsets before anything dereferences wordText(): offset 0 first,
    // strictly increasing, in bounds, and every word NUL-terminated (word i ends
    // at the byte before offset i+1; the last word at the last text byte).
    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
    // Every word font names a font, and every slot one of them.
    bool fontsValid = true;
    for (uint8_t f = 0; f < fontCount; f++) fontsValid = fontsValid && block->wordFontsArr()[f].fontId != 0;
    for (uint16_t i = 0; fontCount != 0 && i < wc; i++)
      fontsValid = fontsValid && block->fontSlotsArr()[i] <= fontCount;
    if (!fontsValid) {
      LOG_ERR("TXB", "Deserialization failed: corrupt word fonts");
      return nullptr;
    }
  }

  // Style: the render slice only (see serialize).
  RenderStyle& renderStyle = block->renderStyle;
  serialization::readPod(file, renderStyle.alignment);
  serialization::readPod(file, renderStyle.fontSizeMultiplier);
  serialization::readPod(file, renderStyle.headingFontId);

  return block;
}

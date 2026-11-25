#pragma once

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <memory>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "clippings/ClippingText.h"
#include "components/themes/BaseTheme.h"

// Word i's font: headings and CSS font-size runs can differ from the section font.
// Single source of truth for a word's on-screen box, shared by live selection and saved clipping highlights.
inline int clippingWordFont(const GfxRenderer& renderer, const int fontId, const TextBlock& block, const uint16_t i) {
  return TextBlock::renderFontId(renderer, block.wordFontId(i, fontId), fontId);
}

// Yields the text of the line's words that use one font; other words are skipped (null).
struct ClippingLineWords {
  const GfxRenderer* renderer;
  const TextBlock* block;
  int fontId;
  int wordFont;

  static const char* get(const void* ctx, const uint32_t index) {
    const auto& self = *static_cast<const ClippingLineWords*>(ctx);
    if (clippingWordFont(*self.renderer, self.fontId, *self.block, static_cast<uint16_t>(index)) != self.wordFont) {
      return nullptr;
    }
    return self.block->wordText(static_cast<uint16_t>(index));
  }
};

// Ink extent per word font for one line, measured lazily and once: the union over the line's unique glyphs, so every
// word box on the line shares one height and any script is measured. Create one per line, outside the word loop.
// rowGap is the distance to the next line on the page (INT_MAX for the last line): boxes are cut to it so they
// never overlap, however tall the script's marks are.
struct ClippingLineInk {
  explicit ClippingLineInk(const int rowGap = INT_MAX) : rowGap(rowGap) {}
  int rowGap;
  std::vector<std::pair<int, GfxRenderer::InkBounds>> byFont;

  GfxRenderer::InkBounds get(const GfxRenderer& renderer, const int fontId, const TextBlock& block, const int wordFont,
                             const int pitch) {
    for (const auto& entry : byFont) {
      if (entry.first == wordFont) return entry.second;
    }
    // SD fonts: load the line's glyph metrics in one prewarm instead of faulting each glyph from the file.
    if (renderer.isSdCardFont(wordFont)) {
      uint8_t styleMask = 0;
      for (uint16_t w = 0; w < block.wordCount(); ++w) {
        styleMask |= static_cast<uint8_t>(1U << (static_cast<uint8_t>(block.wordStyle(w)) & 0x03));
      }
      const ClippingLineWords words{&renderer, &block, fontId, wordFont};
      renderer.ensureSdGlyphMetrics(wordFont, &ClippingLineWords::get, &words, block.wordCount(), styleMask);
    }
    // Unique (style, codepoint) keys, so repeated letters are looked up once.
    std::vector<uint32_t> keys;
    for (uint16_t w = 0; w < block.wordCount(); ++w) {
      if (clippingWordFont(renderer, fontId, block, w) != wordFont) continue;
      const uint32_t style = static_cast<uint32_t>(block.wordStyle(w) & ~EpdFontFamily::UNDERLINE) << 24;
      const auto* p = reinterpret_cast<const unsigned char*>(block.wordText(w));
      while (const uint32_t cp = utf8NextCodepoint(&p)) keys.push_back(style | cp);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    // Nothing measurable (glyphs not loaded yet): neutral box of one pitch from the top, never shifted.
    GfxRenderer::InkBounds ink{0, pitch, false};
    for (const uint32_t key : keys) {
      const auto b = renderer.getGlyphInkBounds(wordFont, key & 0xFFFFFF, static_cast<EpdFontFamily::Style>(key >> 24));
      if (!b.measured) continue;
      ink = ink.measured ? GfxRenderer::InkBounds{std::min(ink.top, b.top), std::max(ink.bottom, b.bottom), true} : b;
    }
    byFont.emplace_back(wordFont, ink);
    return ink;
  }
};

// Distance to the next line on the page, so the line's boxes can be cut to the row. INT_MAX when none follows.
inline int clippingRowGap(const std::vector<std::unique_ptr<PageElement>>& elements, const PageLine& line) {
  int gap = INT_MAX;
  for (const auto& element : elements) {
    if (element->getTag() != TAG_PageLine || element->yPos <= line.yPos) continue;
    gap = std::min(gap, static_cast<int>(element->yPos - line.yPos));
  }
  return gap;
}

inline Rect clippingWordRect(const GfxRenderer& renderer, const int fontId, const PageLine& line, const uint16_t i,
                             const int top, const int left, const int wordWidth, ClippingLineInk& lineInk) {
  const auto& block = line.getBlock();
  const int wordFont = clippingWordFont(renderer, fontId, *block, i);
  const char* text = block->wordText(i);
  const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
  const int skipX =
      clippingText::hasEmSpacePrefix(text) ? renderer.getTextAdvanceX(wordFont, "\xe2\x80\x83", style) : 0;
  int width = wordWidth - skipX;
  if (i + 1 < block->wordCount() && block->wordXpos(i + 1) > block->wordXpos(i)) {
    width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i) - skipX));
  }
  const int lineFont = TextBlock::renderFontId(renderer, block->blockFontId(fontId), fontId);
  // Measured ink extent (any script), as the union over the line's words in this font so boxes share one height.
  // Where it is taller than the row (tight spacing, tall marks) the excess is cut half from each side, so the box
  // never overlaps the neighbouring line.
  const int pitch = renderer.getLineHeight(wordFont, SETTINGS.getReaderLineCompression());
  const auto ink = lineInk.get(renderer, fontId, *block, wordFont, pitch);
  const int excess = std::max(0, ink.bottom - ink.top - lineInk.rowGap);
  const int boxTop = ink.top + excess / 2;
  const int boxHeight = ink.bottom - ink.top - excess;
  return Rect{left + line.xPos + block->wordXpos(i) + skipX,
              top + line.yPos + block->getRubyShift(renderer.getFontAscenderSize(lineFont)) +
                  block->wordYOffset(renderer, fontId, i) + boxTop,
              width, boxHeight};
}

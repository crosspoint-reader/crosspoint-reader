#pragma once

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
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

// Ink extent per word font for one line, measured lazily and once: the union over the line's unique glyphs, so every
// word box on the line shares one height and any script is measured. Create one per line, outside the word loop.
struct ClippingLineInk {
  std::vector<std::pair<int, GfxRenderer::InkBounds>> byFont;

  GfxRenderer::InkBounds get(const GfxRenderer& renderer, const int fontId, const TextBlock& block, const int wordFont,
                             const int pitch) {
    for (const auto& entry : byFont) {
      if (entry.first == wordFont) return entry.second;
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
  // Measured ink extent, capped at the word font's line pitch so tight line spacing doesn't overlap adjacent lines.
  // Trim from the top only: descenders stay whole. The word's own font keeps larger runs intact.
  // Ink is the union over the line's words in this font, so boxes share one height and any script is measured.
  const int pitch = renderer.getLineHeight(wordFont, SETTINGS.getReaderLineCompression());
  const auto ink = lineInk.get(renderer, fontId, *block, wordFont, pitch);
  int trim = ink.bottom - ink.top - pitch;
  // Glue to the line above when the gap fits in the font's own headroom above its letters (ink.top, room the
  // ascender reserves for accents): the box grows upwards but never past the font's ascender. Larger gaps stay.
  if (-trim > ink.top) trim = 0;
  return Rect{left + line.xPos + block->wordXpos(i) + skipX,
              top + line.yPos + block->getRubyShift(renderer.getFontAscenderSize(lineFont)) +
                  block->wordYOffset(renderer, fontId, i) + ink.top + trim,
              width, ink.bottom - ink.top - trim};
}

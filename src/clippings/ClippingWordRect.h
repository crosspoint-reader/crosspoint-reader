#pragma once

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "clippings/ClippingText.h"
#include "components/themes/BaseTheme.h"

// Word i's font: headings and CSS font-size runs can differ from the section font.
// Single source of truth for a word's on-screen box, shared by live selection and saved clipping highlights.
inline int clippingWordFont(const GfxRenderer& renderer, const int fontId, const TextBlock& block, const uint16_t i) {
  return TextBlock::renderFontId(renderer, block.wordFontId(i, fontId), fontId);
}

inline Rect clippingWordRect(const GfxRenderer& renderer, const int fontId, const PageLine& line, const uint16_t i,
                             const int top, const int left, const int wordWidth) {
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
  // Measured ink extent, capped (centered) at the line pitch so tight line spacing doesn't overlap adjacent lines.
  const auto ink = renderer.getFontInkBounds(wordFont);
  const int pitch = renderer.getLineHeight(lineFont, SETTINGS.getReaderLineCompression());
  const int trim = std::max(0, ink.bottom - ink.top - pitch);
  return Rect{left + line.xPos + block->wordXpos(i) + skipX,
              top + line.yPos + block->getRubyShift(renderer.getFontAscenderSize(lineFont)) +
                  block->wordYOffset(renderer, fontId, i) + ink.top + trim / 2,
              width, ink.bottom - ink.top - trim};
}

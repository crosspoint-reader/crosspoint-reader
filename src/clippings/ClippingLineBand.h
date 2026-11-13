#pragma once

#include <Epub/Page.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <type_traits>
#include <utility>

#include "clippings/ClippingText.h"

// Vertical extent of a text line's selection/saved-clipping highlights. Shared by ClipSelectionActivity and
// EpubReaderActivity so selection, saved highlights and tap hit-testing always agree.
//
// Each line starts from its glyph band (ascender + descender of each word's own font). The layout spaces lines by
// font size x line spacing, not by the font's advanceY, so bands can overlap at tight spacing. Only overlapping
// lines in the same horizontal flow (e.g. the same table column) constrain each other, and each overlapping pair
// splits at one shared boundary, so neighbouring highlights never overlap and never leave a gap.
namespace clippingLineBand {

struct Band {
  int top = 0;     // inclusive, page coordinates (before margins)
  int bottom = 0;  // exclusive
};

template <typename Renderer, typename Block>
int wordFont(const Renderer& renderer, const int fontId, const Block& block, const uint16_t i) {
  return Block::renderFontId(renderer, block.wordFontId(i, fontId), fontId);
}

// Glyph band of word i, before any neighbour constraint.
template <typename Renderer, typename Line>
Band wordBand(const Renderer& renderer, const int fontId, const Line& line, const uint16_t i) {
  const auto& block = *line.getBlock();
  using Block = std::remove_cv_t<std::remove_reference_t<decltype(block)>>;
  const int lineFont = Block::renderFontId(renderer, block.blockFontId(fontId), fontId);
  const int font = wordFont(renderer, fontId, block, i);
  const int top =
      line.yPos + block.getRubyShift(renderer.getFontAscenderSize(lineFont)) + block.wordYOffset(renderer, fontId, i);
  return {top, top + renderer.getFontAscenderSize(font) + std::abs(renderer.getFontDescenderSize(font))};
}

// Union of the visible words' bands; empty (top == bottom) for a line without visible words, so whitespace-only
// lines never constrain visible text.
template <typename Renderer, typename Line>
Band naturalBand(const Renderer& renderer, const int fontId, const Line& line) {
  const auto* block = line.getBlock();
  if (!block || !block->valid() || block->wordCount() == 0) return {line.yPos, line.yPos};
  Band band{INT_MAX, INT_MIN};
  for (uint16_t i = 0; i < block->wordCount(); ++i) {
    if (!clippingText::hasVisibleText(block->wordText(i))) continue;
    const Band word = wordBand(renderer, fontId, line, i);
    band.top = std::min(band.top, word.top);
    band.bottom = std::max(band.bottom, word.bottom);
  }
  if (band.bottom <= band.top) return {line.yPos, line.yPos};
  return band;
}

// Horizontal extent [left, right) of the line's words, in page coordinates.
template <typename Renderer, typename Line>
std::pair<int, int> horizontalExtent(const Renderer& renderer, const int fontId, const Line& line) {
  const auto& block = *line.getBlock();
  uint16_t last = 0;
  int left = INT_MAX;
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    left = std::min(left, static_cast<int>(block.wordXpos(i)));
    if (block.wordXpos(i) >= block.wordXpos(last)) last = i;
  }
  // Measure only the rightmost-starting word; it determines the right edge.
  const int width = renderer.getTextAdvanceX(wordFont(renderer, fontId, block, last), block.wordText(last),
                                             block.wordStyle(last), block.getBlockStyle().characterSpacing);
  const int start = line.xPos + block.wordXpos(last);
  return {line.xPos + left, std::max(start + 1, start + width)};
}

// The line's band after sharing a boundary with every overlapping line in the same horizontal flow.
template <typename Renderer, typename PageT, typename Line>
Band lineBand(const Renderer& renderer, const int fontId, const PageT& page, const Line& line) {
  const Band natural = naturalBand(renderer, fontId, line);
  Band band = natural;
  if (natural.bottom <= natural.top) return band;
  bool hasExtent = false;
  std::pair<int, int> extent{0, 0};
  bool selfSeen = false;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& other = static_cast<const Line&>(*element);
    if (&other == &line) {
      selfSeen = true;
      continue;
    }
    const auto* block = other.getBlock();
    if (!block || !block->valid() || block->wordCount() == 0) continue;
    const Band neighbour = naturalBand(renderer, fontId, other);
    if (neighbour.bottom <= natural.top || neighbour.top >= natural.bottom) continue;  // no vertical overlap
    if (!hasExtent) {
      extent = horizontalExtent(renderer, fontId, line);
      hasExtent = true;
    }
    const auto otherExtent = horizontalExtent(renderer, fontId, other);
    if (otherExtent.second <= extent.first || otherExtent.first >= extent.second) continue;  // other column
    // Order by band top; ties fall back to page order so both lines agree on which one is above.
    const bool otherAbove = neighbour.top < natural.top || (neighbour.top == natural.top && !selfSeen);
    const Band& upper = otherAbove ? neighbour : natural;
    const Band& lower = otherAbove ? natural : neighbour;
    const int boundary = (lower.top + upper.bottom) / 2;  // same value from both sides of the pair
    if (otherAbove) {
      band.top = std::max(band.top, boundary);
    } else {
      band.bottom = std::min(band.bottom, boundary);
    }
  }
  if (band.bottom <= band.top) band.bottom = band.top + 1;  // keep fully-covered lines tappable
  return band;
}

// Word i's highlight band: its glyph band clipped to the line's shared band.
template <typename Renderer, typename Line>
Band clipWord(const Renderer& renderer, const int fontId, const Line& line, const uint16_t i, const Band& bounds) {
  const Band word = wordBand(renderer, fontId, line, i);
  Band clipped{std::max(word.top, bounds.top), std::min(word.bottom, bounds.bottom)};
  if (clipped.bottom <= clipped.top) clipped = {bounds.top, bounds.top + 1};
  return clipped;
}

}  // namespace clippingLineBand

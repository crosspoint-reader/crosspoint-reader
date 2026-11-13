#pragma once

#include <Epub/Page.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

#include "clippings/ClippingText.h"

// Vertical extent of a text line's selection/saved-clipping highlights. Shared by ClipSelectionActivity and
// EpubReaderActivity so selection, saved highlights and tap hit-testing always agree.
//
// The band hugs the ink of the glyphs actually on the page, measured per line font around the line's baseline, so
// it fits any script (Latin accents, Greek, CJK, Thai, ...) without per-language rules. When that ink is taller
// than the layout's line step (tight line spacing), the band is one step tall and centred on the ink, so stacked
// lines tile without overlapping. Paragraph, margin and padding gaps are never part of a line's step, so they stay
// unhighlighted.
namespace clippingLineBand {

struct Band {
  int top = 0;     // inclusive, page coordinates (before margins)
  int bottom = 0;  // exclusive
};

// Glyph extent around a baseline: pixels above it and below it.
struct Ink {
  int above = INT_MIN;
  int below = INT_MIN;
  bool empty() const { return above == INT_MIN; }
};

// Per-font ink of one page. Fixed capacity (no heap); fonts beyond it fall back to font metrics.
class PageInk {
 public:
  static constexpr size_t MAX_FONTS = 8;

  const Ink* find(const int font) const {
    for (size_t i = 0; i < count_; ++i) {
      if (entries_[i].font == font) return &entries_[i].ink;
    }
    return nullptr;
  }

  // glyphTop/glyphBottom: y of the glyph's top and bottom edge, upwards from the baseline.
  void add(const int font, const int glyphTop, const int glyphBottom) {
    Ink* ink = nullptr;
    for (size_t i = 0; i < count_ && !ink; ++i) {
      if (entries_[i].font == font) ink = &entries_[i].ink;
    }
    if (!ink) {
      if (count_ == MAX_FONTS) return;
      entries_[count_].font = font;
      ink = &entries_[count_++].ink;
    }
    ink->above = std::max(ink->above, glyphTop);
    ink->below = std::max(ink->below, -glyphBottom);
  }

 private:
  struct Entry {
    int font = 0;
    Ink ink;
  };
  Entry entries_[MAX_FONTS];
  size_t count_ = 0;
};

// The font that sets the line's step: a CSS font-size override, else the section font.
template <typename Renderer, typename Block>
int lineFont(const Renderer& renderer, const int fontId, const Block& block) {
  return Block::renderFontId(renderer, block.blockFontId(fontId), fontId);
}

// Measures the ink of every visible glyph on the page, keyed by its line's font. Superscript/subscript words are
// skipped: they are scaled and shifted, and the line's band is sized by its regular text.
template <typename Renderer, typename PageT>
PageInk measurePage(const Renderer& renderer, const int fontId, const PageT& page) {
  PageInk ink;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine&>(*element).getBlock();
    if (!block || !block->valid()) continue;
    using Block = std::remove_cv_t<std::remove_reference_t<decltype(*block)>>;
    const int font = lineFont(renderer, fontId, *block);
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (!clippingText::hasVisibleText(text)) continue;
      const auto style = static_cast<uint8_t>(block->wordStyle(i));
      if (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) continue;
      const int wordFont = Block::renderFontId(renderer, block->wordFontId(i, fontId), fontId);
      const auto glyphStyle = static_cast<EpdFontFamily::Style>(style & 0x03);
      const auto* cursor = reinterpret_cast<const unsigned char*>(text);
      while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
        int top = 0;
        int bottom = 0;
        if (renderer.getCodepointInk(wordFont, cp, glyphStyle, top, bottom)) ink.add(font, top, bottom);
      }
    }
  }
  return ink;
}

// The line's highlight band. lineCompression is the reader's line spacing, as used by the layout.
template <typename Renderer, typename Line>
Band lineBand(const Renderer& renderer, const int fontId, const Line& line, const float lineCompression,
              const PageInk& pageInk) {
  const auto* block = line.getBlock();
  if (!block || !block->valid()) return {line.yPos, line.yPos + 1};
  const int font = lineFont(renderer, fontId, *block);

  // Same baseline as TextBlock::render and same step as ChapterHtmlSlimParser::addLineToPage.
  const int ascent = block->lineAscent(renderer, fontId);
  const int baseline = line.yPos + block->getRubyShift(ascent) + ascent;
  const int step = renderer.getLineHeight(font, lineCompression) + block->extraAscent(renderer, fontId) +
                   block->getRubyShift(renderer.getFontAscenderSize(font));

  Ink ink;
  if (const Ink* measured = pageInk.find(font)) ink = *measured;
  if (ink.empty()) {
    // Nothing measurable (e.g. glyphs unavailable): fall back to the font's metrics.
    ink = {renderer.getFontAscenderSize(font), std::abs(renderer.getFontDescenderSize(font))};
  }
  const int inkHeight = std::max(1, ink.above + ink.below);
  const int height = std::max(1, step > 0 ? std::min(inkHeight, step) : inkHeight);
  // Centre a step-limited band on the ink, so tight spacing trims top and bottom evenly.
  const int top = baseline - ink.above + (inkHeight - height) / 2;
  return {top, top + height};
}

}  // namespace clippingLineBand

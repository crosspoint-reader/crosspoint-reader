#pragma once

#include <string>
#include "GfxRenderer.h"
#include "components/UITheme.h"

namespace UIHelpers {
/*
UIHelpers - layout helpers for consistent UI across orientations

ContentArea fields:
 - contentX: X origin for main content (excludes left-side hint gutter).
 - contentY: Y origin for main content (excludes top inverted-portrait gutter).
 - contentWidth: Width available for content (excludes left/right gutters).
 - contentHeight: Height available for content (excludes top gutter and reserved bottom space for button hints).
 - hintGutterWidth: Width reserved for side button hints when in landscape.
 - hintGutterHeight: Height reserved for inverted portrait (typically used for rotating hints).
 - isLandscapeCw / isLandscapeCcw: Orientation flags that make it simple to adapt layout to rotation.

Design notes:
 - Use `contentX` and `contentWidth` when drawing lists/menus so highlights and values don't overlap hint gutters.
 - Use `contentY` and `contentHeight` to anchor blocks and avoid overlapping bottom button hints (theme-provided sizes).
 - All helpers are lightweight and take a `GfxRenderer` so they can be used in render() without side-effects.

Examples:

1) Center a title inside the content area and offset it from the top:

  const auto area = UIHelpers::contentAreaForRenderer(renderer);
  auto title = UIHelpers::truncatedTextForContent(renderer, UI_12_FONT_ID, "KOReader Sync", area, EpdFontFamily::BOLD);
  const int titleX = UIHelpers::centeredTextX(renderer, UI_12_FONT_ID, title, area, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, titleX, area.contentY + 15, title.c_str(), true, EpdFontFamily::BOLD);

2) Draw a menu that only highlights the content area:

  const int startY = area.contentY + 75;
  constexpr int rowHeight = 30;
  if (isSelected) {
    renderer.fillRect(area.contentX, displayY, area.contentWidth - 1, rowHeight, true);
  }
  renderer.drawText(UI_10_FONT_ID, area.contentX + 20, displayY, label.c_str());

3) Anchor options above the bottom button hints so they never overlap:

  const int optionHeight = 30;
  const int optionY = area.contentY + std::max(10, area.contentHeight - (optionHeight * 3) - 10);

Helper functions:
 - contentAreaForRenderer(renderer): computes a ContentArea that reserves side and bottom space according to the active Theme.
 - centeredTextX(renderer, fontId, text, area, style): X coordinate that centers text inside `area.contentWidth`.
 - truncatedTextForContent(renderer, fontId, text, area, style): returns a truncated version of `text` that fits inside `area.contentWidth` with modest padding.
*/

struct ContentArea {
  int contentX;
  int contentY;
  int contentWidth;
  int contentHeight;           // Height available for content (reserves bottom button hints)
  int hintGutterWidth;
  int hintGutterHeight;
  bool isLandscapeCw;
  bool isLandscapeCcw;
};

// Compute the content area inside the renderer that reserves space for
// button hint gutters in landscape and the inverted-portrait gutter.
ContentArea contentAreaForRenderer(const GfxRenderer& renderer);

// Returns an X coordinate that will horizontally center `text` within the
// computed content area using the renderer's text metrics.
int centeredTextX(const GfxRenderer& renderer, int fontId, const std::string& text,
                  const ContentArea& area, EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR);

// Convenience wrapper for truncating a string to fit inside the content area.
std::string truncatedTextForContent(const GfxRenderer& renderer, int fontId, const std::string& text,
                                    const ContentArea& area, EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR);
}  // namespace UIHelpers

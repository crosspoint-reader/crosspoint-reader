#pragma once

#include <components/lists/list.h>

#include <algorithm>

// Call after resolving the list's theme geometry and font slots.
inline void fitPluginSubtitleToViewport(const freeink::ui::DrawTarget& target, const int viewportHeight,
                                        freeink::ui::ListProps& props) {
  // FreeInkUI wraps at most 16 lines. Reserve the entire title band so even
  // a wrapped plugin name and a long description stay within one viewport.
  constexpr int MAX_LINES = 16;
  const int labelLines = std::clamp<int>(props.labelText.maxLines, 1, MAX_LINES);
  const int labelHeight = target.lineHeight(props.labelText.font) * labelLines;
  const int valueHeight = target.lineHeight(props.valueText.font);
  const int subtitleHeight = target.lineHeight(props.subtitleText.font);
  const int padding = std::max<int>(props.rowPaddingY, 0) * 2;
  const int available = viewportHeight - std::max(labelHeight, valueHeight) - padding;
  props.subtitleText.maxLines = subtitleHeight > 0 ? std::clamp(available / subtitleHeight, 1, MAX_LINES) : 1;
}

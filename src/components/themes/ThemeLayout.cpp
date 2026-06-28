#include "ThemeLayout.h"

#include <FreeInkUILayout.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

int themeLayoutTokenSize(const ThemeMetrics& metrics, const std::string& token) {
  if (token == "topPadding") return metrics.topPadding;
  if (token == "header") return metrics.headerHeight;
  if (token == "tabBar" || token == "tabs") return metrics.tabBarHeight;
  if (token == "footer" || token == "buttons" || token == "buttonHints") return metrics.buttonHintsHeight;
  if (token == "row") return metrics.listRowHeight;
  if (token == "subtitleRow") return metrics.listWithSubtitleRowHeight;
  if (token == "menuRow") return metrics.menuRowHeight;
  if (token == "recents") return metrics.homeCoverTileHeight;
  if (token == "cover") return metrics.homeCoverHeight;
  if (token == "verticalSpacing" || token == "gap") return metrics.verticalSpacing;
  if (token == "progress") return metrics.progressBarHeight;
  return 0;
}

namespace {

freeink::ui::Axis toUiAxis(ThemeLayoutAxis axis) {
  return axis == ThemeLayoutAxis::Row ? freeink::ui::Axis::Row : freeink::ui::Axis::Column;
}

freeink::ui::LayoutLength toUiLength(const ThemeLayoutNode& node, const ThemeMetrics& metrics) {
  if (node.sizeType == ThemeLayoutSizeType::Fixed) {
    return freeink::ui::LayoutLength::fixed(std::max(0, node.size));
  }

  if (node.sizeType == ThemeLayoutSizeType::Token) {
    return freeink::ui::LayoutLength::fixed(std::max(0, themeLayoutTokenSize(metrics, node.sizeToken)));
  }

  return freeink::ui::LayoutLength::flexible(static_cast<uint8_t>(std::max(1, node.flex)));
}

struct LayoutTreeStorage {
  static constexpr size_t kMaxNodes = 64;

  freeink::ui::LayoutNode nodes[kMaxNodes];
  size_t used = 0;
  bool overflow = false;

  freeink::ui::LayoutNode* allocate(size_t count) {
    if (count == 0) return nullptr;
    if (used + count > kMaxNodes) {
      overflow = true;
      return nullptr;
    }
    auto* out = &nodes[used];
    used += count;
    return out;
  }
};

freeink::ui::LayoutNode toUiNode(const ThemeLayoutNode& node, const ThemeMetrics& metrics, LayoutTreeStorage& storage) {
  freeink::ui::LayoutNode out;
  out.id = node.id.empty() ? nullptr : node.id.c_str();
  out.axis = toUiAxis(node.axis);
  out.gap = static_cast<int16_t>(std::max(0, node.gap));
  out.length = toUiLength(node, metrics);

  const uint8_t childCount = static_cast<uint8_t>(std::min<size_t>(node.children.size(), UINT8_MAX));
  if (childCount == 0) return out;

  auto* children = storage.allocate(childCount);
  if (children == nullptr) return out;
  for (uint8_t i = 0; i < childCount; ++i) {
    children[i] = toUiNode(node.children[i], metrics, storage);
  }
  out.children = children;
  out.childCount = childCount;
  return out;
}

}  // namespace

void layoutThemeSlots(const ThemeLayoutNode& node, Rect rect, const ThemeMetrics& metrics, ThemeLayoutSlots& slots) {
  slots.clear();
  LayoutTreeStorage storage;
  const freeink::ui::LayoutNode uiNode = toUiNode(node, metrics, storage);
  freeink::ui::layoutTree(uiNode, freeink::ui::LayoutRect{rect.x, rect.y, rect.width, rect.height},
                          [&](const char* id, freeink::ui::LayoutRect slot) {
                            if (id != nullptr && id[0] != '\0') {
                              slots.push(id, Rect{static_cast<int>(slot.x), static_cast<int>(slot.y),
                                                  static_cast<int>(slot.width), static_cast<int>(slot.height)});
                            }
                          });
}

Rect findThemeSlot(const ThemeLayoutSlots& slots, const std::string& id) {
  for (size_t i = 0; i < slots.count; ++i) {
    const auto& slot = slots.items[i];
    if (slot.id != nullptr && std::strcmp(slot.id, id.c_str()) == 0) return slot.rect;
  }
  return Rect{};
}

Rect normalizeThemeHeaderSlot(Rect rect, const ThemeMetrics& metrics) {
  if (rect.y == 0 && metrics.topPadding > 0 && rect.height > metrics.topPadding) {
    rect.y += metrics.topPadding;
    rect.height -= metrics.topPadding;
  }
  return rect;
}

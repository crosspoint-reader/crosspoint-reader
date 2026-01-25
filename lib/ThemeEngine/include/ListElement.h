#pragma once

#include <map>
#include <vector>

#include "BasicElements.h"
#include "UIElement.h"

namespace ThemeEngine {

// --- List ---
// Supports vertical, horizontal, and grid layouts
class List : public Container {
 public:
  enum class Direction { Vertical, Horizontal };
  enum class LayoutMode { List, Grid };

 private:
  std::string source;          // Data source name (e.g., "MainMenu", "FileList")
  std::string itemTemplateId;  // ID of the template element
  int itemWidth = 0;           // Explicit item width (0 = auto)
  int itemHeight = 0;          // Explicit item height (0 = auto from template)
  int scrollOffset = 0;        // Scroll position for long lists
  int visibleItems = -1;       // Max visible items (-1 = auto)
  int spacing = 0;             // Gap between items
  int columns = 1;             // Number of columns (for grid mode)
  Direction direction = Direction::Vertical;
  LayoutMode layoutMode = LayoutMode::List;

  // Template element reference (resolved after loading)
  UIElement* itemTemplate = nullptr;

 public:
  List(const std::string& id) : Container(id) {}

  ElementType getType() const override { return ElementType::List; }

  void setSource(const std::string& s) {
    source = s;
    markDirty();
  }

  const std::string& getSource() const { return source; }

  void setItemTemplateId(const std::string& id) {
    itemTemplateId = id;
    markDirty();
  }

  void setItemTemplate(UIElement* elem) {
    itemTemplate = elem;
    markDirty();
  }

  UIElement* getItemTemplate() const { return itemTemplate; }

  void setItemWidth(int w) {
    itemWidth = w;
    markDirty();
  }

  void setItemHeight(int h) {
    itemHeight = h;
    markDirty();
  }

  int getItemHeight() const {
    if (itemHeight > 0) return itemHeight;
    if (itemTemplate) return itemTemplate->getAbsH() > 0 ? itemTemplate->getAbsH() : 45;
    return 45;
  }

  int getItemWidth() const {
    if (itemWidth > 0) return itemWidth;
    if (itemTemplate) return itemTemplate->getAbsW() > 0 ? itemTemplate->getAbsW() : 100;
    return 100;
  }

  void setScrollOffset(int offset) {
    scrollOffset = offset;
    markDirty();
  }

  int getScrollOffset() const { return scrollOffset; }

  void setVisibleItems(int count) {
    visibleItems = count;
    markDirty();
  }

  void setSpacing(int s) {
    spacing = s;
    markDirty();
  }

  void setColumns(int c) {
    columns = c > 0 ? c : 1;
    if (columns > 1) layoutMode = LayoutMode::Grid;
    markDirty();
  }

  void setDirection(Direction d) {
    direction = d;
    markDirty();
  }

  void setDirectionFromString(const std::string& dir) {
    if (dir == "Horizontal" || dir == "horizontal" || dir == "row") {
      direction = Direction::Horizontal;
    } else {
      direction = Direction::Vertical;
    }
    markDirty();
  }

  void setLayoutMode(LayoutMode m) {
    layoutMode = m;
    markDirty();
  }

  // Resolve template reference from element map
  void resolveTemplate(const std::map<std::string, UIElement*>& elements) {
    if (elements.count(itemTemplateId)) {
      itemTemplate = elements.at(itemTemplateId);
    }
  }

  void layout(const ThemeContext& context, int parentX, int parentY, int parentW, int parentH) override {
    // Layout self first (bounds)
    UIElement::layout(context, parentX, parentY, parentW, parentH);

    // Pre-layout the template once with list's dimensions to get item sizes
    // Pass absH so percentage heights in the template work correctly
    if (itemTemplate && itemHeight == 0) {
      itemTemplate->layout(context, absX, absY, absW, absH);
    }
  }

  // Draw is implemented in BasicElements.cpp
  void draw(const GfxRenderer& renderer, const ThemeContext& context) override;
};

}  // namespace ThemeEngine

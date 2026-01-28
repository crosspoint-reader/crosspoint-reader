#pragma once

#include <vector>

#include "BasicElements.h"
#include "ThemeContext.h"
#include "ThemeTypes.h"
#include "UIElement.h"

namespace ThemeEngine {

// --- HStack: Horizontal Stack Layout ---
// Children are arranged horizontally with optional spacing
class HStack : public Container {
 public:
  enum class VAlign { Top, Center, Bottom };

 private:
  int spacing = 0;  // Gap between children
  int padding = 0;  // Internal padding
  VAlign vAlign = VAlign::Top;

 public:
  HStack(const std::string& id) : Container(id) {}
  ElementType getType() const override { return ElementType::HStack; }
  const char* getTypeName() const override { return "HStack"; }

  void setSpacing(int s) {
    spacing = s;
    markDirty();
  }
  void setPadding(int p) {
    padding = p;
    markDirty();
  }
  void setVAlign(VAlign a) {
    vAlign = a;
    markDirty();
  }
  void setVAlignFromString(const std::string& s) {
    if (s == "center" || s == "Center") {
      vAlign = VAlign::Center;
    } else if (s == "bottom" || s == "Bottom") {
      vAlign = VAlign::Bottom;
    } else {
      vAlign = VAlign::Top;
    }
    markDirty();
  }

  void layout(const ThemeContext& context, int parentX, int parentY, int parentW, int parentH) override {
    UIElement::layout(context, parentX, parentY, parentW, parentH);

    int currentX = absX + padding;
    int availableH = absH - 2 * padding;
    int availableW = absW - 2 * padding;

    for (auto child : children) {
      // Let child calculate its preferred size first
      // Pass large parent bounds to avoid clamping issues during size calculation
      child->layout(context, currentX, absY + padding, availableW, availableH);
      int childW = child->getAbsW();
      int childH = child->getAbsH();

      // Extract child's own Y offset (from first layout pass)
      int childYOffset = child->getAbsY() - (absY + padding);

      // Calculate base position based on vertical alignment
      int childY = absY + padding;
      if (childH < availableH) {
        switch (vAlign) {
          case VAlign::Center:
            childY = absY + padding + (availableH - childH) / 2;
            break;
          case VAlign::Bottom:
            childY = absY + padding + (availableH - childH);
            break;
          case VAlign::Top:
          default:
            childY = absY + padding;
            break;
        }
      }

      // Add child's own Y offset to the calculated position
      childY += childYOffset;

      child->layout(context, currentX, childY, childW, childH);
      currentX += childW + spacing;
      availableW -= (childW + spacing);
      if (availableW < 0) availableW = 0;
    }
  }
};

// --- VStack: Vertical Stack Layout ---
// Children are arranged vertically with optional spacing
class VStack : public Container {
 public:
  enum class HAlign { Left, Center, Right };

 private:
  int spacing = 0;
  int padding = 0;
  HAlign hAlign = HAlign::Left;

 public:
  VStack(const std::string& id) : Container(id) {}
  ElementType getType() const override { return ElementType::VStack; }
  const char* getTypeName() const override { return "VStack"; }

  void setSpacing(int s) {
    spacing = s;
    markDirty();
  }
  void setPadding(int p) {
    padding = p;
    markDirty();
  }
  void setHAlign(HAlign a) {
    hAlign = a;
    markDirty();
  }
  void setHAlignFromString(const std::string& s) {
    if (s == "center" || s == "Center") {
      hAlign = HAlign::Center;
    } else if (s == "right" || s == "Right") {
      hAlign = HAlign::Right;
    } else {
      hAlign = HAlign::Left;
    }
    markDirty();
  }

  void layout(const ThemeContext& context, int parentX, int parentY, int parentW, int parentH) override {
    UIElement::layout(context, parentX, parentY, parentW, parentH);

    int currentY = absY + padding;
    int availableW = absW - 2 * padding;
    int availableH = absH - 2 * padding;

    for (auto child : children) {
      // Pass large parent bounds to avoid clamping issues during size calculation
      child->layout(context, absX + padding, currentY, availableW, availableH);
      int childW = child->getAbsW();
      int childH = child->getAbsH();

      // Extract child's own X offset (from first layout pass)
      int childXOffset = child->getAbsX() - (absX + padding);

      // Calculate base position based on horizontal alignment
      int childX = absX + padding;
      if (childW < availableW) {
        switch (hAlign) {
          case HAlign::Center:
            childX = absX + padding + (availableW - childW) / 2;
            break;
          case HAlign::Right:
            childX = absX + padding + (availableW - childW);
            break;
          case HAlign::Left:
          default:
            childX = absX + padding;
            break;
        }
      }

      // Add child's own X offset to the calculated position
      childX += childXOffset;

      child->layout(context, childX, currentY, childW, childH);
      currentY += childH + spacing;
      availableH -= (childH + spacing);
      if (availableH < 0) availableH = 0;
    }
  }
};

// --- Grid: Grid Layout ---
// Children arranged in a grid with specified columns
class Grid : public Container {
  int columns = 2;
  int rowSpacing = 10;
  int colSpacing = 10;
  int padding = 0;

 public:
  Grid(const std::string& id) : Container(id) {}
  ElementType getType() const override { return ElementType::Grid; }
  const char* getTypeName() const override { return "Grid"; }

  void setColumns(int c) {
    columns = c > 0 ? c : 1;
    markDirty();
  }
  void setRowSpacing(int s) {
    rowSpacing = s;
    markDirty();
  }
  void setColSpacing(int s) {
    colSpacing = s;
    markDirty();
  }
  void setPadding(int p) {
    padding = p;
    markDirty();
  }

  void layout(const ThemeContext& context, int parentX, int parentY, int parentW, int parentH) override {
    UIElement::layout(context, parentX, parentY, parentW, parentH);

    if (children.empty()) return;

    int availableW = absW - 2 * padding - (columns - 1) * colSpacing;
    int cellW = availableW / columns;
    int availableH = absH - 2 * padding;

    int row = 0, col = 0;
    int currentY = absY + padding;
    int maxRowHeight = 0;

    for (auto child : children) {
      int cellX = absX + padding + col * (cellW + colSpacing);

      // Pass cell dimensions to avoid clamping issues
      child->layout(context, cellX, currentY, cellW, availableH);
      int childH = child->getAbsH();
      if (childH > maxRowHeight) maxRowHeight = childH;

      col++;
      if (col >= columns) {
        col = 0;
        row++;
        currentY += maxRowHeight + rowSpacing;
        availableH -= (maxRowHeight + rowSpacing);
        if (availableH < 0) availableH = 0;
        maxRowHeight = 0;
      }
    }
  }
};

// --- Badge: Small overlay text/indicator ---
class Badge : public UIElement {
  Expression textExpr;
  Expression bgColorExpr;
  Expression fgColorExpr;
  int fontId = 0;
  int paddingH = 8;  // Horizontal padding
  int paddingV = 4;  // Vertical padding
  int borderRadius = 0;

 public:
  Badge(const std::string& id) : UIElement(id) {
    bgColorExpr = Expression::parse("0x00");  // Black background
    fgColorExpr = Expression::parse("0xFF");  // White text
  }

  ElementType getType() const override { return ElementType::Badge; }
  const char* getTypeName() const override { return "Badge"; }

  void setText(const std::string& expr) {
    textExpr = Expression::parse(expr);
    markDirty();
  }
  void setBgColor(const std::string& expr) {
    bgColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setFgColor(const std::string& expr) {
    fgColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setFont(int fid) {
    fontId = fid;
    markDirty();
  }
  void setPaddingH(int p) {
    paddingH = p;
    markDirty();
  }
  void setPaddingV(int p) {
    paddingV = p;
    markDirty();
  }

  void setBorderRadius(int r) {
    borderRadius = r;
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override {
    if (!isVisible(context)) return;

    std::string text = context.evaluatestring(textExpr);
    if (text.empty()) {
      markClean();
      return;
    }

    // Calculate badge size based on text content - always auto-sizes
    int textW = renderer.getTextWidth(fontId, text.c_str());
    int textH = renderer.getLineHeight(fontId);
    int badgeW = textW + 2 * paddingH;
    int badgeH = textH + 2 * paddingV;

    // Badge always auto-sizes to content
    int drawW = badgeW;
    int drawH = badgeH;

    // Position the badge within its container
    // If absW/absH are set, use them as bounding box for alignment
    int drawX = absX;
    int drawY = absY;

    // Right-align badge within bounding box if width is specified
    if (absW > 0 && absW > drawW) {
      drawX = absX + absW - drawW;
    }
    // Vertically center badge within bounding box if height is specified
    if (absH > 0 && absH > drawH) {
      drawY = absY + (absH - drawH) / 2;
    }

    // Draw background
    std::string bgStr = context.evaluatestring(bgColorExpr);
    uint8_t bgColor = Color::parse(bgStr).value;
    if (borderRadius > 0) {
      if (bgColor == 0x00) {
        renderer.fillRoundedRect(drawX, drawY, drawW, drawH, borderRadius, true);
      } else if (bgColor >= 0xF0) {
        renderer.fillRoundedRect(drawX, drawY, drawW, drawH, borderRadius, false);
      } else {
        renderer.fillRoundedRectDithered(drawX, drawY, drawW, drawH, borderRadius, bgColor);
      }
    } else {
      renderer.fillRect(drawX, drawY, drawW, drawH, bgColor == 0x00);
    }

    // Draw border for contrast (only if not black background)
    if (bgColor != 0x00) {
      if (borderRadius > 0) {
        renderer.drawRoundedRect(drawX, drawY, drawW, drawH, borderRadius, true);
      } else {
        renderer.drawRect(drawX, drawY, drawW, drawH, true);
      }
    }

    // Draw text centered within the badge
    std::string fgStr = context.evaluatestring(fgColorExpr);
    uint8_t fgColor = Color::parse(fgStr).value;
    int textX = drawX + paddingH;
    int textY = drawY + paddingV;
    renderer.drawText(fontId, textX, textY, text.c_str(), fgColor == 0x00);

    markClean();
  }
};

// --- Toggle: On/Off Switch ---
// Fully themable toggle with track and knob
// Supports rounded or square appearance based on BorderRadius
class Toggle : public UIElement {
  Expression valueExpr;      // Boolean expression for on/off state
  Expression onColorExpr;    // Track color when ON
  Expression offColorExpr;   // Track color when OFF
  Expression knobColorExpr;  // Knob color (optional, defaults to opposite of track)
  int trackWidth = 44;
  int trackHeight = 24;
  int knobSize = 20;
  int borderRadius = 0;  // 0 = square, >0 = rounded (use trackHeight/2 for pill shape)
  int knobRadius = 0;    // Knob corner radius

 public:
  Toggle(const std::string& id) : UIElement(id) {
    valueExpr = Expression::parse("false");
    onColorExpr = Expression::parse("0x00");   // Black when on
    offColorExpr = Expression::parse("0xCC");  // Light gray when off
  }

  ElementType getType() const override { return ElementType::Toggle; }
  const char* getTypeName() const override { return "Toggle"; }

  void setValue(const std::string& expr) {
    valueExpr = Expression::parse(expr);
    markDirty();
  }
  void setOnColor(const std::string& expr) {
    onColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setOffColor(const std::string& expr) {
    offColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setKnobColor(const std::string& expr) {
    knobColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setTrackWidth(int w) {
    trackWidth = w;
    markDirty();
  }
  void setTrackHeight(int h) {
    trackHeight = h;
    markDirty();
  }
  void setKnobSize(int s) {
    knobSize = s;
    markDirty();
  }
  void setBorderRadius(int r) {
    borderRadius = r;
    markDirty();
  }
  void setKnobRadius(int r) {
    knobRadius = r;
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override {
    if (!isVisible(context)) return;

    // Evaluate the value - handle simple variable references directly
    bool isOn = false;
    std::string rawExpr = valueExpr.rawExpr;

    // If it's a simple {variable} reference, resolve it directly
    if (rawExpr.size() > 2 && rawExpr.front() == '{' && rawExpr.back() == '}') {
      std::string varName = rawExpr.substr(1, rawExpr.size() - 2);
      // Trim whitespace
      size_t start = varName.find_first_not_of(" \t");
      size_t end = varName.find_last_not_of(" \t");
      if (start != std::string::npos) {
        varName = varName.substr(start, end - start + 1);
      }
      isOn = context.getAnyAsBool(varName, false);
    } else {
      isOn = context.evaluateBool(rawExpr);
    }

    // Get track color based on state
    std::string colorStr = isOn ? context.evaluatestring(onColorExpr) : context.evaluatestring(offColorExpr);
    uint8_t trackColor = Color::parse(colorStr).value;

    // Calculate track position (centered vertically in bounding box)
    int trackX = absX;
    int trackY = absY + (absH - trackHeight) / 2;

    // Calculate effective border radius (capped at half height for pill shape)
    int effectiveRadius = borderRadius;
    if (effectiveRadius > trackHeight / 2) {
      effectiveRadius = trackHeight / 2;
    }

    // Draw track
    if (effectiveRadius > 0) {
      // Rounded track
      if (trackColor == 0x00) {
        renderer.fillRoundedRect(trackX, trackY, trackWidth, trackHeight, effectiveRadius, true);
      } else if (trackColor >= 0xF0) {
        renderer.fillRoundedRect(trackX, trackY, trackWidth, trackHeight, effectiveRadius, false);
        renderer.drawRoundedRect(trackX, trackY, trackWidth, trackHeight, effectiveRadius, true);
      } else {
        renderer.fillRoundedRectDithered(trackX, trackY, trackWidth, trackHeight, effectiveRadius, trackColor);
        renderer.drawRoundedRect(trackX, trackY, trackWidth, trackHeight, effectiveRadius, true);
      }
    } else {
      // Square track
      if (trackColor == 0x00) {
        renderer.fillRect(trackX, trackY, trackWidth, trackHeight, true);
      } else if (trackColor >= 0xF0) {
        renderer.fillRect(trackX, trackY, trackWidth, trackHeight, false);
        renderer.drawRect(trackX, trackY, trackWidth, trackHeight, true);
      } else {
        renderer.fillRectDithered(trackX, trackY, trackWidth, trackHeight, trackColor);
        renderer.drawRect(trackX, trackY, trackWidth, trackHeight, true);
      }
    }

    // Calculate knob position
    int knobMargin = (trackHeight - knobSize) / 2;
    int knobX = isOn ? (trackX + trackWidth - knobSize - knobMargin) : (trackX + knobMargin);
    int knobY = trackY + knobMargin;

    // Determine knob color
    bool knobBlack;
    if (!knobColorExpr.empty()) {
      std::string knobStr = context.evaluatestring(knobColorExpr);
      uint8_t knobColor = Color::parse(knobStr).value;
      knobBlack = (knobColor == 0x00);
    } else {
      // Default: knob is opposite color of track
      knobBlack = (trackColor >= 0x80);
    }

    // Calculate effective knob radius
    int effectiveKnobRadius = knobRadius;
    if (effectiveKnobRadius > knobSize / 2) {
      effectiveKnobRadius = knobSize / 2;
    }

    // Draw knob
    if (effectiveKnobRadius > 0) {
      renderer.fillRoundedRect(knobX, knobY, knobSize, knobSize, effectiveKnobRadius, knobBlack);
      if (!knobBlack) {
        renderer.drawRoundedRect(knobX, knobY, knobSize, knobSize, effectiveKnobRadius, true);
      }
    } else {
      renderer.fillRect(knobX, knobY, knobSize, knobSize, knobBlack);
      if (!knobBlack) {
        renderer.drawRect(knobX, knobY, knobSize, knobSize, true);
      }
    }

    markClean();
  }
};

// --- TabBar: Horizontal tab selection ---
class TabBar : public Container {
  Expression selectedExpr;  // Currently selected tab index or name
  int tabSpacing = 0;
  int padding = 0;
  int indicatorHeight = 3;
  bool showIndicator = true;

 public:
  TabBar(const std::string& id) : Container(id) {}
  ElementType getType() const override { return ElementType::TabBar; }
  const char* getTypeName() const override { return "TabBar"; }

  void setSelected(const std::string& expr) {
    selectedExpr = Expression::parse(expr);
    markDirty();
  }
  void setTabSpacing(int s) {
    tabSpacing = s;
    markDirty();
  }
  void setPadding(int p) {
    padding = p;
    markDirty();
  }
  void setIndicatorHeight(int h) {
    indicatorHeight = h;
    markDirty();
  }
  void setShowIndicator(bool show) {
    showIndicator = show;
    markDirty();
  }

  void layout(const ThemeContext& context, int parentX, int parentY, int parentW, int parentH) override {
    UIElement::layout(context, parentX, parentY, parentW, parentH);

    if (children.empty()) return;

    // Distribute tabs evenly
    int numTabs = children.size();
    int totalSpacing = (numTabs - 1) * tabSpacing;
    int availableW = absW - 2 * padding - totalSpacing;
    int tabW = availableW / numTabs;
    int currentX = absX + padding;

    for (size_t i = 0; i < children.size(); i++) {
      children[i]->layout(context, currentX, absY, tabW, absH - indicatorHeight);
      currentX += tabW + tabSpacing;
    }
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override {
    if (!isVisible(context)) return;

    // Draw background if set
    if (hasBg) {
      std::string colStr = context.evaluatestring(bgColorExpr);
      uint8_t color = Color::parse(colStr).value;
      renderer.fillRect(absX, absY, absW, absH, color == 0x00);
    }

    // Draw children (tab labels)
    for (auto child : children) {
      child->draw(renderer, context);
    }

    // Draw selection indicator
    if (showIndicator && !children.empty()) {
      std::string selStr = context.evaluatestring(selectedExpr);
      int selectedIdx = 0;
      if (!selStr.empty()) {
        // Try to parse as number
        try {
          selectedIdx = std::stoi(selStr);
        } catch (...) {
          selectedIdx = 0;
        }
      }

      if (selectedIdx >= 0 && selectedIdx < static_cast<int>(children.size())) {
        UIElement* tab = children[selectedIdx];
        int indX = tab->getAbsX();
        int indY = absY + absH - indicatorHeight;
        int indW = tab->getAbsW();
        renderer.fillRect(indX, indY, indW, indicatorHeight, true);
      }
    }

    markClean();
  }
};

// --- Icon: Small symbolic image ---
// Can be a built-in icon name or a path to a BMP
class Icon : public UIElement {
  Expression srcExpr;  // Icon name or path
  Expression colorExpr;
  int iconSize = 24;

  // Built-in icon names and their simple representations
  // In a real implementation, these would be actual bitmap data

 public:
  Icon(const std::string& id) : UIElement(id) {
    colorExpr = Expression::parse("0x00");  // Black by default
  }

  ElementType getType() const override { return ElementType::Icon; }
  const char* getTypeName() const override { return "Icon"; }

  void setSrc(const std::string& expr) {
    srcExpr = Expression::parse(expr);
    markDirty();
  }
  void setColorExpr(const std::string& expr) {
    colorExpr = Expression::parse(expr);
    markDirty();
  }
  void setIconSize(int s) {
    iconSize = s;
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override;
};

// --- ScrollIndicator: Visual scroll position ---
class ScrollIndicator : public UIElement {
  Expression positionExpr;  // 0.0 to 1.0
  Expression totalExpr;     // Total items
  Expression visibleExpr;   // Visible items
  int trackWidth = 4;

 public:
  ScrollIndicator(const std::string& id) : UIElement(id) {
    positionExpr = Expression::parse("0");
    totalExpr = Expression::parse("1");
    visibleExpr = Expression::parse("1");
  }

  ElementType getType() const override { return ElementType::ScrollIndicator; }
  const char* getTypeName() const override { return "ScrollIndicator"; }

  void setPosition(const std::string& expr) {
    positionExpr = Expression::parse(expr);
    markDirty();
  }
  void setTotal(const std::string& expr) {
    totalExpr = Expression::parse(expr);
    markDirty();
  }
  void setVisibleCount(const std::string& expr) {
    visibleExpr = Expression::parse(expr);
    markDirty();
  }
  void setTrackWidth(int w) {
    trackWidth = w;
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override {
    if (!isVisible(context)) return;

    // Get values
    std::string posStr = context.evaluatestring(positionExpr);
    std::string totalStr = context.evaluatestring(totalExpr);
    std::string visStr = context.evaluatestring(visibleExpr);

    float position = posStr.empty() ? 0 : std::stof(posStr);
    int total = totalStr.empty() ? 1 : std::stoi(totalStr);
    int visible = visStr.empty() ? 1 : std::stoi(visStr);

    if (total <= visible) {
      // No need to show scrollbar
      markClean();
      return;
    }

    // Draw track
    int trackX = absX + (absW - trackWidth) / 2;
    renderer.drawRect(trackX, absY, trackWidth, absH, true);

    // Calculate thumb size and position
    float ratio = static_cast<float>(visible) / static_cast<float>(total);
    int thumbH = static_cast<int>(absH * ratio);
    if (thumbH < 20) thumbH = 20;  // Minimum thumb size

    int maxScroll = total - visible;
    float scrollRatio = maxScroll > 0 ? position / maxScroll : 0;
    int thumbY = absY + static_cast<int>((absH - thumbH) * scrollRatio);

    // Draw thumb
    renderer.fillRect(trackX, thumbY, trackWidth, thumbH, true);

    markClean();
  }
};

}  // namespace ThemeEngine

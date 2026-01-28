#pragma once

#include <Bitmap.h>
#include <SDCardManager.h>

#include <vector>

#include "ThemeContext.h"
#include "ThemeTypes.h"
#include "UIElement.h"

namespace ThemeEngine {

// Safe integer parsing (no exceptions)
inline int parseIntSafe(const std::string& s, int defaultVal = 0) {
  if (s.empty()) return defaultVal;
  char* end;
  long val = strtol(s.c_str(), &end, 10);
  return (end != s.c_str()) ? static_cast<int>(val) : defaultVal;
}

// Safe float parsing (no exceptions)
inline float parseFloatSafe(const std::string& s, float defaultVal = 0.0f) {
  if (s.empty()) return defaultVal;
  char* end;
  float val = strtof(s.c_str(), &end);
  return (end != s.c_str()) ? val : defaultVal;
}

// --- Container ---
class Container : public UIElement {
 protected:
  std::vector<UIElement*> children;
  Expression bgColorExpr;
  bool hasBg = false;
  bool border = false;
  Expression borderExpr;  // Dynamic border based on expression
  int padding = 0;        // Inner padding for children
  int borderRadius = 0;   // Corner radius (for future rounded rect support)

 public:
  explicit Container(const std::string& id) : UIElement(id), bgColorExpr(Expression::parse("0xFF")) {}
  virtual ~Container() = default;

  Container* asContainer() override { return this; }

  ElementType getType() const override { return ElementType::Container; }
  const char* getTypeName() const override { return "Container"; }

  void addChild(UIElement* child) { children.push_back(child); }

  void clearChildren() { children.clear(); }

  const std::vector<UIElement*>& getChildren() const { return children; }

  void setBackgroundColorExpr(const std::string& expr) {
    bgColorExpr = Expression::parse(expr);
    hasBg = true;
    markDirty();
  }

  void setBorder(bool enable) {
    border = enable;
    markDirty();
  }

  void setBorderExpr(const std::string& expr) {
    borderExpr = Expression::parse(expr);
    markDirty();
  }

  bool hasBorderExpr() const { return !borderExpr.empty(); }

  void setPadding(int p) {
    padding = p;
    markDirty();
  }

  int getPadding() const { return padding; }

  void setBorderRadius(int r) {
    borderRadius = r;
    markDirty();
  }

  int getBorderRadius() const { return borderRadius; }

  void layout(const ThemeContext& context, int parentX, int parentY, int parentW, int parentH) override {
    UIElement::layout(context, parentX, parentY, parentW, parentH);
    // Children are laid out with padding offset
    int childX = absX + padding;
    int childY = absY + padding;
    int childW = absW - 2 * padding;
    int childH = absH - 2 * padding;
    for (auto child : children) {
      child->layout(context, childX, childY, childW, childH);
    }
  }

  void markDirty() override {
    UIElement::markDirty();
    for (auto child : children) {
      child->markDirty();
    }
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override;
};

// --- Rectangle ---
class Rectangle : public UIElement {
  bool fill = false;
  Expression fillExpr;  // Dynamic fill based on expression
  Expression colorExpr;
  int borderRadius = 0;

 public:
  explicit Rectangle(const std::string& id) : UIElement(id), colorExpr(Expression::parse("0x00")) {}
  ElementType getType() const override { return ElementType::Rectangle; }
  const char* getTypeName() const override { return "Rectangle"; }

  void setFill(bool f) {
    fill = f;
    markDirty();
  }

  void setFillExpr(const std::string& expr) {
    fillExpr = Expression::parse(expr);
    markDirty();
  }

  void setColorExpr(const std::string& c) {
    colorExpr = Expression::parse(c);
    markDirty();
  }

  void setBorderRadius(int r) {
    borderRadius = r;
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override;
};

// --- Label ---
class Label : public UIElement {
 public:
  enum class Alignment { Left, Center, Right };

 private:
  Expression textExpr;
  int fontId = 0;
  Alignment alignment = Alignment::Left;
  Expression colorExpr;
  int maxLines = 1;      // For multi-line support
  bool ellipsis = true;  // Truncate with ... if too long

 public:
  explicit Label(const std::string& id) : UIElement(id), colorExpr(Expression::parse("0x00")) {}
  ElementType getType() const override { return ElementType::Label; }
  const char* getTypeName() const override { return "Label"; }

  void setText(const std::string& expr) {
    textExpr = Expression::parse(expr);
    markDirty();
  }
  void setFont(int fid) {
    fontId = fid;
    markDirty();
  }
  void setAlignment(Alignment a) {
    alignment = a;
    markDirty();
  }
  void setCentered(bool c) {
    alignment = c ? Alignment::Center : Alignment::Left;
    markDirty();
  }
  void setColorExpr(const std::string& c) {
    colorExpr = Expression::parse(c);
    markDirty();
  }
  void setMaxLines(int lines) {
    maxLines = lines;
    markDirty();
  }
  void setEllipsis(bool e) {
    ellipsis = e;
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override;
};

// --- BitmapElement ---
class BitmapElement : public UIElement {
  Expression srcExpr;
  bool scaleToFit = true;
  bool preserveAspect = true;
  int borderRadius = 0;

 public:
  explicit BitmapElement(const std::string& id) : UIElement(id) {
    cacheable = true;  // Bitmaps benefit from caching
  }
  ElementType getType() const override { return ElementType::Bitmap; }
  const char* getTypeName() const override { return "Bitmap"; }

  void setSrc(const std::string& src) {
    srcExpr = Expression::parse(src);
    invalidateCache();
  }

  void setScaleToFit(bool scale) {
    scaleToFit = scale;
    invalidateCache();
  }

  void setPreserveAspect(bool preserve) {
    preserveAspect = preserve;
    invalidateCache();
  }

  void setBorderRadius(int r) {
    borderRadius = r;
    // Radius doesn't affect cache key unless we baked it in (we don't currently),
    // but we should redraw.
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override;
};

// --- ProgressBar ---
class ProgressBar : public UIElement {
  Expression valueExpr;    // Current value (0-100 or 0-max)
  Expression maxExpr;      // Max value (default 100)
  Expression fgColorExpr;  // Foreground color
  Expression bgColorExpr;  // Background color
  bool showBorder = true;
  int borderWidth = 1;

 public:
  explicit ProgressBar(const std::string& id)
      : UIElement(id),
        valueExpr(Expression::parse("0")),
        maxExpr(Expression::parse("100")),
        fgColorExpr(Expression::parse("0x00")),  // Black fill
        bgColorExpr(Expression::parse("0xFF"))   // White background
  {}

  ElementType getType() const override { return ElementType::ProgressBar; }
  const char* getTypeName() const override { return "ProgressBar"; }

  void setValue(const std::string& expr) {
    valueExpr = Expression::parse(expr);
    markDirty();
  }
  void setMax(const std::string& expr) {
    maxExpr = Expression::parse(expr);
    markDirty();
  }
  void setFgColor(const std::string& expr) {
    fgColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setBgColor(const std::string& expr) {
    bgColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setShowBorder(bool show) {
    showBorder = show;
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override {
    if (!isVisible(context)) return;

    std::string valStr = context.evaluatestring(valueExpr);
    std::string maxStr = context.evaluatestring(maxExpr);

    int value = parseIntSafe(valStr, 0);
    int maxVal = parseIntSafe(maxStr, 100);
    if (maxVal <= 0) maxVal = 100;

    float ratio = static_cast<float>(value) / static_cast<float>(maxVal);
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    // Draw background
    std::string bgStr = context.evaluatestring(bgColorExpr);
    uint8_t bgColor = Color::parse(bgStr).value;
    renderer.fillRect(absX, absY, absW, absH, bgColor == 0x00);

    // Draw filled portion
    int fillWidth = static_cast<int>(absW * ratio);
    if (fillWidth > 0) {
      std::string fgStr = context.evaluatestring(fgColorExpr);
      uint8_t fgColor = Color::parse(fgStr).value;
      renderer.fillRect(absX, absY, fillWidth, absH, fgColor == 0x00);
    }

    // Draw border
    if (showBorder) {
      renderer.drawRect(absX, absY, absW, absH, true);
    }

    markClean();
  }
};

// --- Divider (horizontal or vertical line) ---
class Divider : public UIElement {
  Expression colorExpr;
  bool horizontal = true;
  int thickness = 1;

 public:
  explicit Divider(const std::string& id) : UIElement(id), colorExpr(Expression::parse("0x00")) {}

  ElementType getType() const override { return ElementType::Divider; }
  const char* getTypeName() const override { return "Divider"; }

  void setColorExpr(const std::string& expr) {
    colorExpr = Expression::parse(expr);
    markDirty();
  }
  void setHorizontal(bool h) {
    horizontal = h;
    markDirty();
  }
  void setThickness(int t) {
    thickness = t;
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override {
    if (!isVisible(context)) return;

    std::string colStr = context.evaluatestring(colorExpr);
    uint8_t color = Color::parse(colStr).value;
    bool black = (color == 0x00);

    if (horizontal) {
      for (int i = 0; i < thickness && i < absH; i++) {
        renderer.drawLine(absX, absY + i, absX + absW - 1, absY + i, black);
      }
    } else {
      for (int i = 0; i < thickness && i < absW; i++) {
        renderer.drawLine(absX + i, absY, absX + i, absY + absH - 1, black);
      }
    }

    markClean();
  }
};

// --- BatteryIcon ---
class BatteryIcon : public UIElement {
  Expression valueExpr;
  Expression colorExpr;

 public:
  explicit BatteryIcon(const std::string& id)
      : UIElement(id), valueExpr(Expression::parse("0")), colorExpr(Expression::parse("0x00")) {
    // Black by default
  }

  ElementType getType() const override { return ElementType::BatteryIcon; }
  const char* getTypeName() const override { return "BatteryIcon"; }

  void setValue(const std::string& expr) {
    valueExpr = Expression::parse(expr);
    markDirty();
  }

  void setColor(const std::string& expr) {
    colorExpr = Expression::parse(expr);
    markDirty();
  }

  void draw(const GfxRenderer& renderer, const ThemeContext& context) override {
    if (!isVisible(context)) return;

    std::string valStr = context.evaluatestring(valueExpr);
    int percentage = parseIntSafe(valStr, 0);

    std::string colStr = context.evaluatestring(colorExpr);
    uint8_t color = Color::parse(colStr).value;
    bool black = (color == 0x00);

    constexpr int batteryWidth = 15;
    constexpr int batteryHeight = 12;

    int x = absX;
    int y = absY;

    if (absW > batteryWidth) x += (absW - batteryWidth) / 2;
    if (absH > batteryHeight) y += (absH - batteryHeight) / 2;

    renderer.drawLine(x + 1, y, x + batteryWidth - 3, y, black);
    renderer.drawLine(x + 1, y + batteryHeight - 1, x + batteryWidth - 3, y + batteryHeight - 1, black);
    renderer.drawLine(x, y + 1, x, y + batteryHeight - 2, black);
    renderer.drawLine(x + batteryWidth - 2, y + 1, x + batteryWidth - 2, y + batteryHeight - 2, black);

    renderer.drawPixel(x + batteryWidth - 1, y + 3, black);
    renderer.drawPixel(x + batteryWidth - 1, y + batteryHeight - 4, black);
    renderer.drawLine(x + batteryWidth - 0, y + 4, x + batteryWidth - 0, y + batteryHeight - 5, black);

    if (percentage > 0) {
      int filledWidth = percentage * (batteryWidth - 5) / 100 + 1;
      if (filledWidth > batteryWidth - 5) {
        filledWidth = batteryWidth - 5;
      }
      renderer.fillRect(x + 2, y + 2, filledWidth, batteryHeight - 4, black);
    }

    markClean();
  }
};

}  // namespace ThemeEngine

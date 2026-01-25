#pragma once

#include "ThemeContext.h"
#include "ThemeTypes.h"
#include "UIElement.h"
#include <Bitmap.h>
#include <SDCardManager.h>
#include <vector>

namespace ThemeEngine {

// --- Container ---
class Container : public UIElement {
protected:
  std::vector<UIElement *> children;
  Expression bgColorExpr;
  bool hasBg = false;
  bool border = false;
  Expression borderExpr; // Dynamic border based on expression

public:
  Container(const std::string &id) : UIElement(id) {
    bgColorExpr = Expression::parse("0xFF");
  }
  virtual ~Container() {
    for (auto child : children)
      delete child;
  }

  Container *asContainer() override { return this; }

  ElementType getType() const override { return ElementType::Container; }

  void addChild(UIElement *child) { children.push_back(child); }

  const std::vector<UIElement *> &getChildren() const { return children; }

  void setBackgroundColorExpr(const std::string &expr) {
    bgColorExpr = Expression::parse(expr);
    hasBg = true;
    markDirty();
  }

  void setBorder(bool enable) {
    border = enable;
    markDirty();
  }

  void setBorderExpr(const std::string &expr) {
    borderExpr = Expression::parse(expr);
    markDirty();
  }

  bool hasBorderExpr() const { return !borderExpr.empty(); }

  void layout(const ThemeContext &context, int parentX, int parentY,
              int parentW, int parentH) override {
    UIElement::layout(context, parentX, parentY, parentW, parentH);
    for (auto child : children) {
      child->layout(context, absX, absY, absW, absH);
    }
  }

  void markDirty() override {
    UIElement::markDirty();
    for (auto child : children) {
      child->markDirty();
    }
  }

  void draw(const GfxRenderer &renderer, const ThemeContext &context) override {
    if (!isVisible(context))
      return;

    if (hasBg) {
      std::string colStr = context.evaluatestring(bgColorExpr);
      uint8_t color = Color::parse(colStr).value;
      renderer.fillRect(absX, absY, absW, absH, color == 0x00);
    }

    // Handle dynamic border expression
    bool drawBorder = border;
    if (hasBorderExpr()) {
      drawBorder = context.evaluateBool(borderExpr.rawExpr);
    }

    if (drawBorder) {
      renderer.drawRect(absX, absY, absW, absH, true);
    }

    for (auto child : children) {
      child->draw(renderer, context);
    }

    markClean();
  }
};

// --- Rectangle ---
class Rectangle : public UIElement {
  bool fill = false;
  Expression fillExpr; // Dynamic fill based on expression
  Expression colorExpr;

public:
  Rectangle(const std::string &id) : UIElement(id) {
    colorExpr = Expression::parse("0x00");
  }
  ElementType getType() const override { return ElementType::Rectangle; }

  void setFill(bool f) {
    fill = f;
    markDirty();
  }

  void setFillExpr(const std::string &expr) {
    fillExpr = Expression::parse(expr);
    markDirty();
  }

  void setColorExpr(const std::string &c) {
    colorExpr = Expression::parse(c);
    markDirty();
  }

  void draw(const GfxRenderer &renderer, const ThemeContext &context) override {
    if (!isVisible(context))
      return;

    std::string colStr = context.evaluatestring(colorExpr);
    uint8_t color = Color::parse(colStr).value;
    bool black = (color == 0x00);

    bool shouldFill = fill;
    if (!fillExpr.empty()) {
      shouldFill = context.evaluateBool(fillExpr.rawExpr);
    }

    if (shouldFill) {
      renderer.fillRect(absX, absY, absW, absH, black);
    } else {
      renderer.drawRect(absX, absY, absW, absH, black);
    }

    markClean();
  }
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
  int maxLines = 1; // For multi-line support
  bool ellipsis = true; // Truncate with ... if too long

public:
  Label(const std::string &id) : UIElement(id) {
    colorExpr = Expression::parse("0x00");
  }
  ElementType getType() const override { return ElementType::Label; }

  void setText(const std::string &expr) {
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
  void setColorExpr(const std::string &c) {
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

  void draw(const GfxRenderer &renderer, const ThemeContext &context) override {
    if (!isVisible(context))
      return;

    std::string finalText = context.evaluatestring(textExpr);
    if (finalText.empty()) {
      markClean();
      return;
    }

    std::string colStr = context.evaluatestring(colorExpr);
    uint8_t color = Color::parse(colStr).value;
    bool black = (color == 0x00);

    int textWidth = renderer.getTextWidth(fontId, finalText.c_str());
    int lineHeight = renderer.getLineHeight(fontId);

    // Truncate if needed
    if (ellipsis && textWidth > absW && absW > 0) {
      finalText = renderer.truncatedText(fontId, finalText.c_str(), absW);
      textWidth = renderer.getTextWidth(fontId, finalText.c_str());
    }

    int drawX = absX;
    int drawY = absY;

    // Vertical centering
    if (absH > 0 && lineHeight > 0) {
      drawY = absY + (absH - lineHeight) / 2;
    }

    // Horizontal alignment
    if (alignment == Alignment::Center && absW > 0) {
      drawX = absX + (absW - textWidth) / 2;
    } else if (alignment == Alignment::Right && absW > 0) {
      drawX = absX + absW - textWidth;
    }

    renderer.drawText(fontId, drawX, drawY, finalText.c_str(), black);
    markClean();
  }
};

// --- BitmapElement ---
class BitmapElement : public UIElement {
  Expression srcExpr;
  bool scaleToFit = true;
  bool preserveAspect = true;

public:
  BitmapElement(const std::string &id) : UIElement(id) {
    cacheable = true; // Bitmaps benefit from caching
  }
  ElementType getType() const override { return ElementType::Bitmap; }

  void setSrc(const std::string &src) {
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

  void draw(const GfxRenderer &renderer, const ThemeContext &context) override;
};

// --- ProgressBar ---
class ProgressBar : public UIElement {
  Expression valueExpr;   // Current value (0-100 or 0-max)
  Expression maxExpr;     // Max value (default 100)
  Expression fgColorExpr; // Foreground color
  Expression bgColorExpr; // Background color
  bool showBorder = true;
  int borderWidth = 1;

public:
  ProgressBar(const std::string &id) : UIElement(id) {
    valueExpr = Expression::parse("0");
    maxExpr = Expression::parse("100");
    fgColorExpr = Expression::parse("0x00"); // Black fill
    bgColorExpr = Expression::parse("0xFF"); // White background
  }

  ElementType getType() const override { return ElementType::ProgressBar; }

  void setValue(const std::string &expr) {
    valueExpr = Expression::parse(expr);
    markDirty();
  }
  void setMax(const std::string &expr) {
    maxExpr = Expression::parse(expr);
    markDirty();
  }
  void setFgColor(const std::string &expr) {
    fgColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setBgColor(const std::string &expr) {
    bgColorExpr = Expression::parse(expr);
    markDirty();
  }
  void setShowBorder(bool show) {
    showBorder = show;
    markDirty();
  }

  void draw(const GfxRenderer &renderer, const ThemeContext &context) override {
    if (!isVisible(context))
      return;

    std::string valStr = context.evaluatestring(valueExpr);
    std::string maxStr = context.evaluatestring(maxExpr);

    int value = valStr.empty() ? 0 : std::stoi(valStr);
    int maxVal = maxStr.empty() ? 100 : std::stoi(maxStr);
    if (maxVal <= 0)
      maxVal = 100;

    float ratio = static_cast<float>(value) / static_cast<float>(maxVal);
    if (ratio < 0)
      ratio = 0;
    if (ratio > 1)
      ratio = 1;

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
  Divider(const std::string &id) : UIElement(id) {
    colorExpr = Expression::parse("0x00");
  }

  ElementType getType() const override { return ElementType::Divider; }

  void setColorExpr(const std::string &expr) {
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

  void draw(const GfxRenderer &renderer, const ThemeContext &context) override {
    if (!isVisible(context))
      return;

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

} // namespace ThemeEngine

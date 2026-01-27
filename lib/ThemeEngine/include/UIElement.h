#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "ThemeContext.h"
#include "ThemeTypes.h"

namespace ThemeEngine {

class Container;  // Forward declaration

class UIElement {
 public:
  int getAbsX() const { return absX; }
  int getAbsY() const { return absY; }
  int getAbsW() const { return absW; }
  int getAbsH() const { return absH; }
  const std::string& getId() const { return id; }

 protected:
  std::string id;
  Dimension x, y, width, height;
  Expression visibleExpr;
  bool visibleExprIsStatic = true;  // True if visibility doesn't depend on data

  // Recomputed every layout pass
  int absX = 0, absY = 0, absW = 0, absH = 0;

  // Caching support
  bool cacheable = false;   // Set true for expensive elements like bitmaps
  bool cacheValid = false;  // Is the cached render still valid?
  uint8_t* cachedRender = nullptr;
  size_t cachedRenderSize = 0;
  int cachedX = 0, cachedY = 0, cachedW = 0, cachedH = 0;

  // Dirty tracking
  bool dirty = true;  // Needs redraw

  bool isVisible(const ThemeContext& context) const {
    if (visibleExpr.empty()) return true;
    return context.evaluateBool(visibleExpr.rawExpr);
  }

 public:
  UIElement(const std::string& id) : id(id), visibleExpr(Expression::parse("true")) {}

  virtual ~UIElement() {
    if (cachedRender) {
      free(cachedRender);
      cachedRender = nullptr;
    }
  }

  void setX(Dimension val) {
    x = val;
    markDirty();
  }
  void setY(Dimension val) {
    y = val;
    markDirty();
  }
  void setWidth(Dimension val) {
    width = val;
    markDirty();
  }
  void setHeight(Dimension val) {
    height = val;
    markDirty();
  }
  void setVisibleExpr(const std::string& expr) {
    visibleExpr = Expression::parse(expr);
    // Check if expression contains variables
    visibleExprIsStatic =
        (expr == "true" || expr == "false" || expr == "1" || expr == "0" || expr.find('{') == std::string::npos);
    markDirty();
  }

  void setCacheable(bool val) { cacheable = val; }
  bool isCacheable() const { return cacheable; }

  virtual void markDirty() {
    dirty = true;
    cacheValid = false;
  }

  void markClean() { dirty = false; }
  bool isDirty() const { return dirty; }

  // Invalidate cache (called when dependent data changes)
  void invalidateCache() {
    cacheValid = false;
    dirty = true;
  }

  // Calculate absolute position based on parent
  virtual void layout(const ThemeContext& context, int parentX, int parentY, int parentW, int parentH) {
    int newX = parentX + x.resolve(parentW);
    int newY = parentY + y.resolve(parentH);
    int newW = width.resolve(parentW);
    int newH = height.resolve(parentH);

    // Clamp to parent bounds
    if (newX >= parentX + parentW) newX = parentX + parentW - 1;
    if (newY >= parentY + parentH) newY = parentY + parentH - 1;

    int maxX = parentX + parentW;
    int maxY = parentY + parentH;

    if (newX + newW > maxX) newW = maxX - newX;
    if (newY + newH > maxY) newH = maxY - newY;

    if (newW < 0) newW = 0;
    if (newH < 0) newH = 0;

    // Check if position changed
    if (newX != absX || newY != absY || newW != absW || newH != absH) {
      absX = newX;
      absY = newY;
      absW = newW;
      absH = newH;
      markDirty();
    }
  }

  virtual Container* asContainer() { return nullptr; }

  enum class ElementType {
    Base,
    Container,
    Rectangle,
    Label,
    Bitmap,
    List,
    ProgressBar,
    Divider,
    // Layout elements
    HStack,
    VStack,
    Grid,
    // Advanced elements
    Badge,
    Toggle,
    TabBar,
    Icon,
    BatteryIcon,
    ScrollIndicator
  };

  virtual ElementType getType() const { return ElementType::Base; }

  int getLayoutHeight() const { return absH; }
  int getLayoutWidth() const { return absW; }

  // Get bounding rect for this element
  Rect getBounds() const { return Rect(absX, absY, absW, absH); }

  // Main draw method - handles caching automatically
  virtual void draw(const GfxRenderer& renderer, const ThemeContext& context) = 0;

 protected:
  // Cache the rendered output
  bool cacheRender(const GfxRenderer& renderer) {
    if (cachedRender) {
      free(cachedRender);
      cachedRender = nullptr;
    }

    cachedRender = renderer.captureRegion(absX, absY, absW, absH, &cachedRenderSize);
    if (cachedRender) {
      cachedX = absX;
      cachedY = absY;
      cachedW = absW;
      cachedH = absH;
      cacheValid = true;
      return true;
    }
    return false;
  }

  // Restore from cache
  bool restoreFromCache(const GfxRenderer& renderer) const {
    if (!cacheValid || !cachedRender) return false;
    if (absX != cachedX || absY != cachedY || absW != cachedW || absH != cachedH) return false;

    renderer.restoreRegion(cachedRender, absX, absY, absW, absH);
    return true;
  }
};

}  // namespace ThemeEngine

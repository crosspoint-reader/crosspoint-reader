#pragma once

#include <cstdint>

// Text alignment options matching CSS text-align property
enum class TextAlign : uint8_t { None = 0, Left = 1, Right = 2, Center = 3, Justify = 4 };

// CSS length unit types
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3 };

// Represents a CSS length value with its unit, allowing deferred resolution to pixels
struct CssLength {
  float value = 0.0f;
  CssUnit unit = CssUnit::Pixels;

  CssLength() = default;
  CssLength(const float v, const CssUnit u) : value(v), unit(u) {}

  // Convenience constructor for pixel values (most common case)
  explicit CssLength(const float pixels) : value(pixels) {}

  // Resolve to pixels given the current em size (font line height)
  [[nodiscard]] float toPixels(const float emSize) const {
    switch (unit) {
      case CssUnit::Em:
      case CssUnit::Rem:
        return value * emSize;
      case CssUnit::Points:
        return value * 1.33f;  // Approximate pt to px conversion
      default:
        return value;
    }
  }

  // Resolve to int16_t pixels (for BlockStyle fields)
  [[nodiscard]] int16_t toPixelsInt16(const float emSize) const { return static_cast<int16_t>(toPixels(emSize)); }
};

// Font style options matching CSS font-style property
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1 };

// Text decoration options
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1 };

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint16_t alignment : 1;
  uint16_t fontStyle : 1;
  uint16_t fontWeight : 1;
  uint16_t decoration : 1;
  uint16_t indent : 1;
  uint16_t marginTop : 1;
  uint16_t marginBottom : 1;
  uint16_t marginLeft : 1;
  uint16_t marginRight : 1;
  uint16_t paddingTop : 1;
  uint16_t paddingBottom : 1;
  uint16_t paddingLeft : 1;
  uint16_t paddingRight : 1;
  uint16_t reserved : 3;

  CssPropertyFlags()
      : alignment(0),
        fontStyle(0),
        fontWeight(0),
        decoration(0),
        indent(0),
        marginTop(0),
        marginBottom(0),
        marginLeft(0),
        marginRight(0),
        paddingTop(0),
        paddingBottom(0),
        paddingLeft(0),
        paddingRight(0),
        reserved(0) {}

  [[nodiscard]] bool anySet() const {
    return alignment || fontStyle || fontWeight || decoration || indent || marginTop || marginBottom || marginLeft ||
           marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight;
  }

  void clearAll() {
    alignment = fontStyle = fontWeight = decoration = indent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
  }
};

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
struct CssStyle {
  TextAlign alignment = TextAlign::None;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration decoration = CssTextDecoration::None;

  CssLength indent;         // First-line indent (deferred resolution)
  CssLength marginTop;      // Vertical spacing before block
  CssLength marginBottom;   // Vertical spacing after block
  CssLength marginLeft;     // Horizontal spacing left of block
  CssLength marginRight;    // Horizontal spacing right of block
  CssLength paddingTop;     // Padding before
  CssLength paddingBottom;  // Padding after
  CssLength paddingLeft;    // Padding left
  CssLength paddingRight;   // Padding right

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined
  void applyOver(const CssStyle& base) {
    if (base.defined.alignment) {
      alignment = base.alignment;
      defined.alignment = 1;
    }
    if (base.defined.fontStyle) {
      fontStyle = base.fontStyle;
      defined.fontStyle = 1;
    }
    if (base.defined.fontWeight) {
      fontWeight = base.fontWeight;
      defined.fontWeight = 1;
    }
    if (base.defined.decoration) {
      decoration = base.decoration;
      defined.decoration = 1;
    }
    if (base.defined.indent) {
      indent = base.indent;
      defined.indent = 1;
    }
    if (base.defined.marginTop) {
      marginTop = base.marginTop;
      defined.marginTop = 1;
    }
    if (base.defined.marginBottom) {
      marginBottom = base.marginBottom;
      defined.marginBottom = 1;
    }
    if (base.defined.marginLeft) {
      marginLeft = base.marginLeft;
      defined.marginLeft = 1;
    }
    if (base.defined.marginRight) {
      marginRight = base.marginRight;
      defined.marginRight = 1;
    }
    if (base.defined.paddingTop) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.defined.paddingBottom) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
    }
    if (base.defined.paddingLeft) {
      paddingLeft = base.paddingLeft;
      defined.paddingLeft = 1;
    }
    if (base.defined.paddingRight) {
      paddingRight = base.paddingRight;
      defined.paddingRight = 1;
    }
  }

  // Compatibility accessors for existing code that uses hasX pattern
  [[nodiscard]] bool hasTextAlign() const { return defined.alignment; }
  [[nodiscard]] bool hasFontStyle() const { return defined.fontStyle; }
  [[nodiscard]] bool hasFontWeight() const { return defined.fontWeight; }
  [[nodiscard]] bool hasTextDecoration() const { return defined.decoration; }
  [[nodiscard]] bool hasTextIndent() const { return defined.indent; }
  [[nodiscard]] bool hasMarginTop() const { return defined.marginTop; }
  [[nodiscard]] bool hasMarginBottom() const { return defined.marginBottom; }
  [[nodiscard]] bool hasMarginLeft() const { return defined.marginLeft; }
  [[nodiscard]] bool hasMarginRight() const { return defined.marginRight; }
  [[nodiscard]] bool hasPaddingTop() const { return defined.paddingTop; }
  [[nodiscard]] bool hasPaddingBottom() const { return defined.paddingBottom; }
  [[nodiscard]] bool hasPaddingLeft() const { return defined.paddingLeft; }
  [[nodiscard]] bool hasPaddingRight() const { return defined.paddingRight; }

  // Merge another style (alias for applyOver for compatibility)
  void merge(const CssStyle& other) { applyOver(other); }

  void reset() {
    alignment = TextAlign::None;
    fontStyle = CssFontStyle::Normal;
    fontWeight = CssFontWeight::Normal;
    decoration = CssTextDecoration::None;
    indent = CssLength{};
    marginTop = marginBottom = marginLeft = marginRight = CssLength{};
    paddingTop = paddingBottom = paddingLeft = paddingRight = CssLength{};
    defined.clearAll();
  }
};

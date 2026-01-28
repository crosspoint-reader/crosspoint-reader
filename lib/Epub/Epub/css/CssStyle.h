#pragma once

#include <cstdint>

// Text alignment options matching CSS text-align property
enum class TextAlign : uint8_t { None = 0, Left = 1, Right = 2, Center = 3, Justify = 4 };

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
  uint16_t paddingTop : 1;
  uint16_t paddingBottom : 1;
  uint16_t reserved : 7;

  CssPropertyFlags()
      : alignment(0),
        fontStyle(0),
        fontWeight(0),
        decoration(0),
        indent(0),
        marginTop(0),
        marginBottom(0),
        paddingTop(0),
        paddingBottom(0),
        reserved(0) {}

  [[nodiscard]] bool anySet() const {
    return alignment || fontStyle || fontWeight || decoration || indent || marginTop || marginBottom || paddingTop ||
           paddingBottom;
  }

  void clearAll() {
    alignment = fontStyle = fontWeight = decoration = indent = 0;
    marginTop = marginBottom = paddingTop = paddingBottom = 0;
  }
};

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
struct CssStyle {
  TextAlign alignment = TextAlign::None;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration decoration = CssTextDecoration::None;

  float indentPixels = 0.0f;  // First-line indent in pixels
  int8_t marginTop = 0;       // Vertical spacing before block (in lines, 0-2)
  int8_t marginBottom = 0;    // Vertical spacing after block (in lines, 0-2)
  int8_t paddingTop = 0;      // Padding before (in lines, 0-2)
  int8_t paddingBottom = 0;   // Padding after (in lines, 0-2)

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
      indentPixels = base.indentPixels;
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
    if (base.defined.paddingTop) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.defined.paddingBottom) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
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
  [[nodiscard]] bool hasPaddingTop() const { return defined.paddingTop; }
  [[nodiscard]] bool hasPaddingBottom() const { return defined.paddingBottom; }

  // Merge another style (alias for applyOver for compatibility)
  void merge(const CssStyle& other) { applyOver(other); }

  void reset() {
    alignment = TextAlign::None;
    fontStyle = CssFontStyle::Normal;
    fontWeight = CssFontWeight::Normal;
    decoration = CssTextDecoration::None;
    indentPixels = 0.0f;
    marginTop = marginBottom = paddingTop = paddingBottom = 0;
    defined.clearAll();
  }
};

#pragma once

#include <cstdint>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4 };

// Represents a CSS length value with its unit, allowing deferred resolution to pixels
struct CssLength {
  float value = 0.0f;
  CssUnit unit = CssUnit::Pixels;

  CssLength() = default;
  CssLength(const float v, const CssUnit u) : value(v), unit(u) {}

  // Convenience constructor for pixel values (most common case)
  explicit CssLength(const float pixels) : value(pixels) {}

  // Returns true if this length can be resolved to pixels with the given context.
  // Percentage units require a non-zero containerWidth to resolve.
  [[nodiscard]] bool isResolvable(const float containerWidth = 0) const {
    return unit != CssUnit::Percent || containerWidth > 0;
  }

  // Resolve to pixels given the current em size (font line height)
  // containerWidth is needed for percentage units (e.g. viewport width)
  [[nodiscard]] float toPixels(const float emSize, const float containerWidth = 0) const {
    switch (unit) {
      case CssUnit::Em:
      case CssUnit::Rem:
        return value * emSize;
      case CssUnit::Points:
        return value * 1.33f;  // Approximate pt to px conversion
      case CssUnit::Percent:
        return value * containerWidth / 100.0f;
      default:
        return value;
    }
  }

  // Resolve to int16_t pixels (for BlockStyle fields)
  [[nodiscard]] int16_t toPixelsInt16(const float emSize, const float containerWidth = 0) const {
    return static_cast<int16_t>(toPixels(emSize, containerWidth));
  }
};

// Font style options matching CSS font-style property
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1 };

// Text decoration options
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1 };

// Display options - only None and Block are relevant for e-ink rendering
enum class CssDisplay : uint8_t { Block = 0, None = 1 };

// Border style options - controls table border rendering
enum class CssBorderStyle : uint8_t { Solid = 0, None = 1 };

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint32_t textAlign : 1;
  uint32_t fontStyle : 1;
  uint32_t fontWeight : 1;
  uint32_t textDecoration : 1;
  uint32_t textIndent : 1;
  uint32_t marginTop : 1;
  uint32_t marginBottom : 1;
  uint32_t marginLeft : 1;
  uint32_t marginRight : 1;
  uint32_t paddingTop : 1;
  uint32_t paddingBottom : 1;
  uint32_t paddingLeft : 1;
  uint32_t paddingRight : 1;
  uint32_t imageHeight : 1;
  uint32_t imageWidth : 1;
  uint32_t display : 1;
  uint32_t borderTopStyle : 1;
  uint32_t borderBottomStyle : 1;
  uint32_t borderLeftStyle : 1;
  uint32_t borderRightStyle : 1;
  // Set when the corresponding border-*-width was explicitly parsed as 0.
  // A side is invisible when style==Solid but this bit is set.
  uint32_t borderTopWidthZero : 1;
  uint32_t borderBottomWidthZero : 1;
  uint32_t borderLeftWidthZero : 1;
  uint32_t borderRightWidthZero : 1;
  // Set when the corresponding border-*-width was explicitly parsed as non-zero.
  // Used during cascade to clear a lower-priority WidthZero flag.
  uint32_t borderTopWidthSet : 1;
  uint32_t borderBottomWidthSet : 1;
  uint32_t borderLeftWidthSet : 1;
  uint32_t borderRightWidthSet : 1;

  CssPropertyFlags()
      : textAlign(0),
        fontStyle(0),
        fontWeight(0),
        textDecoration(0),
        textIndent(0),
        marginTop(0),
        marginBottom(0),
        marginLeft(0),
        marginRight(0),
        paddingTop(0),
        paddingBottom(0),
        paddingLeft(0),
        paddingRight(0),
        imageHeight(0),
        imageWidth(0),
        display(0),
        borderTopStyle(0),
        borderBottomStyle(0),
        borderLeftStyle(0),
        borderRightStyle(0),
        borderTopWidthZero(0),
        borderBottomWidthZero(0),
        borderLeftWidthZero(0),
        borderRightWidthZero(0),
        borderTopWidthSet(0),
        borderBottomWidthSet(0),
        borderLeftWidthSet(0),
        borderRightWidthSet(0) {}

  [[nodiscard]] bool anySet() const {
    return textAlign || fontStyle || fontWeight || textDecoration || textIndent || marginTop || marginBottom ||
           marginLeft || marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight || imageHeight ||
           imageWidth || display || borderTopStyle || borderBottomStyle || borderLeftStyle || borderRightStyle ||
           borderTopWidthZero || borderBottomWidthZero || borderLeftWidthZero || borderRightWidthZero ||
           borderTopWidthSet || borderBottomWidthSet || borderLeftWidthSet || borderRightWidthSet;
  }

  void clearAll() {
    textAlign = fontStyle = fontWeight = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = 0;
    borderTopStyle = borderBottomStyle = borderLeftStyle = borderRightStyle = 0;
    borderTopWidthZero = borderBottomWidthZero = borderLeftWidthZero = borderRightWidthZero = 0;
    borderTopWidthSet = borderBottomWidthSet = borderLeftWidthSet = borderRightWidthSet = 0;
  }
};

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
struct CssStyle {
  CssTextAlign textAlign = CssTextAlign::Left;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration textDecoration = CssTextDecoration::None;

  CssLength textIndent;     // First-line indent (deferred resolution)
  CssLength marginTop;      // Vertical spacing before block
  CssLength marginBottom;   // Vertical spacing after block
  CssLength marginLeft;     // Horizontal spacing left of block
  CssLength marginRight;    // Horizontal spacing right of block
  CssLength paddingTop;     // Padding before
  CssLength paddingBottom;  // Padding after
  CssLength paddingLeft;    // Padding left
  CssLength paddingRight;   // Padding right
  CssLength imageHeight;    // Height for img (e.g. 2em) – width derived from aspect ratio when only height set
  CssLength imageWidth;     // Width for img when both or only width set
  CssDisplay display = CssDisplay::Block;  // display property (Block or None)
  CssBorderStyle borderTopStyle = CssBorderStyle::None;
  CssBorderStyle borderBottomStyle = CssBorderStyle::None;
  CssBorderStyle borderLeftStyle = CssBorderStyle::None;
  CssBorderStyle borderRightStyle = CssBorderStyle::None;

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined
  void applyOver(const CssStyle& base) {
    if (base.hasTextAlign()) {
      textAlign = base.textAlign;
      defined.textAlign = 1;
    }
    if (base.hasFontStyle()) {
      fontStyle = base.fontStyle;
      defined.fontStyle = 1;
    }
    if (base.hasFontWeight()) {
      fontWeight = base.fontWeight;
      defined.fontWeight = 1;
    }
    if (base.hasTextDecoration()) {
      textDecoration = base.textDecoration;
      defined.textDecoration = 1;
    }
    if (base.hasTextIndent()) {
      textIndent = base.textIndent;
      defined.textIndent = 1;
    }
    if (base.hasMarginTop()) {
      marginTop = base.marginTop;
      defined.marginTop = 1;
    }
    if (base.hasMarginBottom()) {
      marginBottom = base.marginBottom;
      defined.marginBottom = 1;
    }
    if (base.hasMarginLeft()) {
      marginLeft = base.marginLeft;
      defined.marginLeft = 1;
    }
    if (base.hasMarginRight()) {
      marginRight = base.marginRight;
      defined.marginRight = 1;
    }
    if (base.hasPaddingTop()) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.hasPaddingBottom()) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
    }
    if (base.hasPaddingLeft()) {
      paddingLeft = base.paddingLeft;
      defined.paddingLeft = 1;
    }
    if (base.hasPaddingRight()) {
      paddingRight = base.paddingRight;
      defined.paddingRight = 1;
    }
    if (base.hasImageHeight()) {
      imageHeight = base.imageHeight;
      defined.imageHeight = 1;
    }
    if (base.hasImageWidth()) {
      imageWidth = base.imageWidth;
      defined.imageWidth = 1;
    }
    if (base.hasDisplay()) {
      display = base.display;
      defined.display = 1;
    }
    if (base.hasBorderTopStyle()) {
      borderTopStyle = base.borderTopStyle;
      defined.borderTopStyle = 1;
    }
    if (base.hasBorderBottomStyle()) {
      borderBottomStyle = base.borderBottomStyle;
      defined.borderBottomStyle = 1;
    }
    if (base.hasBorderLeftStyle()) {
      borderLeftStyle = base.borderLeftStyle;
      defined.borderLeftStyle = 1;
    }
    if (base.hasBorderRightStyle()) {
      borderRightStyle = base.borderRightStyle;
      defined.borderRightStyle = 1;
    }
    // Width flags: propagate from base (inline/overriding style) when explicitly set.
    // WidthSet (non-zero) and WidthZero are mutually exclusive; the incoming style wins.
    if (base.defined.borderTopWidthSet) {
      defined.borderTopWidthSet = 1;
      defined.borderTopWidthZero = 0;
    } else if (base.defined.borderTopWidthZero) {
      defined.borderTopWidthZero = 1;
      defined.borderTopWidthSet = 0;
    }
    if (base.defined.borderBottomWidthSet) {
      defined.borderBottomWidthSet = 1;
      defined.borderBottomWidthZero = 0;
    } else if (base.defined.borderBottomWidthZero) {
      defined.borderBottomWidthZero = 1;
      defined.borderBottomWidthSet = 0;
    }
    if (base.defined.borderLeftWidthSet) {
      defined.borderLeftWidthSet = 1;
      defined.borderLeftWidthZero = 0;
    } else if (base.defined.borderLeftWidthZero) {
      defined.borderLeftWidthZero = 1;
      defined.borderLeftWidthSet = 0;
    }
    if (base.defined.borderRightWidthSet) {
      defined.borderRightWidthSet = 1;
      defined.borderRightWidthZero = 0;
    } else if (base.defined.borderRightWidthZero) {
      defined.borderRightWidthZero = 1;
      defined.borderRightWidthSet = 0;
    }
  }

  [[nodiscard]] bool hasTextAlign() const { return defined.textAlign; }
  [[nodiscard]] bool hasFontStyle() const { return defined.fontStyle; }
  [[nodiscard]] bool hasFontWeight() const { return defined.fontWeight; }
  [[nodiscard]] bool hasTextDecoration() const { return defined.textDecoration; }
  [[nodiscard]] bool hasTextIndent() const { return defined.textIndent; }
  [[nodiscard]] bool hasMarginTop() const { return defined.marginTop; }
  [[nodiscard]] bool hasMarginBottom() const { return defined.marginBottom; }
  [[nodiscard]] bool hasMarginLeft() const { return defined.marginLeft; }
  [[nodiscard]] bool hasMarginRight() const { return defined.marginRight; }
  [[nodiscard]] bool hasPaddingTop() const { return defined.paddingTop; }
  [[nodiscard]] bool hasPaddingBottom() const { return defined.paddingBottom; }
  [[nodiscard]] bool hasPaddingLeft() const { return defined.paddingLeft; }
  [[nodiscard]] bool hasPaddingRight() const { return defined.paddingRight; }
  [[nodiscard]] bool hasImageHeight() const { return defined.imageHeight; }
  [[nodiscard]] bool hasImageWidth() const { return defined.imageWidth; }
  [[nodiscard]] bool hasDisplay() const { return defined.display; }
  [[nodiscard]] bool hasBorderTopStyle() const { return defined.borderTopStyle; }
  [[nodiscard]] bool hasBorderBottomStyle() const { return defined.borderBottomStyle; }
  [[nodiscard]] bool hasBorderLeftStyle() const { return defined.borderLeftStyle; }
  [[nodiscard]] bool hasBorderRightStyle() const { return defined.borderRightStyle; }
  [[nodiscard]] bool hasBorderTopWidthSet() const { return defined.borderTopWidthSet; }
  [[nodiscard]] bool hasBorderBottomWidthSet() const { return defined.borderBottomWidthSet; }
  [[nodiscard]] bool hasBorderLeftWidthSet() const { return defined.borderLeftWidthSet; }
  [[nodiscard]] bool hasBorderRightWidthSet() const { return defined.borderRightWidthSet; }
  // Returns true if the border side is both styled solid AND has a non-zero width.
  [[nodiscard]] bool isBorderTopVisible() const {
    return borderTopStyle != CssBorderStyle::None && !defined.borderTopWidthZero;
  }
  [[nodiscard]] bool isBorderBottomVisible() const {
    return borderBottomStyle != CssBorderStyle::None && !defined.borderBottomWidthZero;
  }
  [[nodiscard]] bool isBorderLeftVisible() const {
    return borderLeftStyle != CssBorderStyle::None && !defined.borderLeftWidthZero;
  }
  [[nodiscard]] bool isBorderRightVisible() const {
    return borderRightStyle != CssBorderStyle::None && !defined.borderRightWidthZero;
  }
  [[nodiscard]] bool hasBorderStyle() const {
    return defined.borderTopStyle || defined.borderBottomStyle || defined.borderLeftStyle || defined.borderRightStyle;
  }

  void reset() {
    textAlign = CssTextAlign::Left;
    fontStyle = CssFontStyle::Normal;
    fontWeight = CssFontWeight::Normal;
    textDecoration = CssTextDecoration::None;
    textIndent = CssLength{};
    marginTop = marginBottom = marginLeft = marginRight = CssLength{};
    paddingTop = paddingBottom = paddingLeft = paddingRight = CssLength{};
    imageHeight = imageWidth = CssLength{};
    display = CssDisplay::Block;
    borderTopStyle = borderBottomStyle = borderLeftStyle = borderRightStyle = CssBorderStyle::None;
    defined.clearAll();
  }
};

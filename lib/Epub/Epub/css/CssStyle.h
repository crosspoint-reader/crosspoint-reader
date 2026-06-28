#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4 };
enum class CssTextDirection : uint8_t { Ltr = 0, Rtl = 1 };

// Represents a CSS length value with its unit, allowing deferred resolution to pixels
struct CssLength {
  // Compact 32-bit layout:
  //   - low 3 bits store CssUnit
  //   - upper 29 bits store a signed decimal fixed-point value at 1e-4 precision
  //
  // The previous float+unit representation consumed 8 bytes per field. This
  // packed form cuts CssLength to 4 bytes and shrinks CssStyle substantially,
  // while keeping common EPUB values (integer and 0.01-step decimals) stable
  // under the integer/truncating pixel math used by layout.
  uint32_t packed = 0;

  CssLength() = default;
  CssLength(const float v, const CssUnit u) : packed(pack(v, u)) {}

  // Convenience constructor for pixel values (most common case)
  explicit CssLength(const float pixels) : packed(pack(pixels, CssUnit::Pixels)) {}

  [[nodiscard]] float value() const { return static_cast<float>(scaledValue()) * kInvScale; }
  [[nodiscard]] CssUnit unit() const { return unpackUnit(packed); }
  [[nodiscard]] uint32_t rawPacked() const { return packed; }
  [[nodiscard]] int32_t scaledValue() const { return unpackScaled(packed); }

  void setValue(const float v) { packed = pack(v, unit()); }
  void setUnit(const CssUnit u) { packed = packScaled(scaledValue(), u); }
  void setRawPacked(const uint32_t raw) { packed = raw; }

  // Returns true if this length can be resolved to pixels with the given context.
  // Percentage units require a non-zero containerWidth to resolve.
  [[nodiscard]] bool isResolvable(const float containerWidth = 0) const {
    return unit() != CssUnit::Percent || containerWidth > 0;
  }

  // Resolve to pixels given the current em size (font line height)
  // containerWidth is needed for percentage units (e.g. viewport width)
  [[nodiscard]] float toPixels(const float emSize, const float containerWidth = 0) const {
    const int32_t scaled = scaledValue();
    switch (unit()) {
      case CssUnit::Em:
      case CssUnit::Rem:
        return static_cast<float>(static_cast<double>(scaled) * static_cast<double>(emSize) / kScale);
      case CssUnit::Points:
        return static_cast<float>((static_cast<int64_t>(scaled) * 133.0) / (kScale * 100.0));  // Approximate pt to px
      case CssUnit::Percent:
        return static_cast<float>(static_cast<double>(scaled) * static_cast<double>(containerWidth) / (kScale * 100.0));
      default:
        return static_cast<float>(scaled) * kInvScale;
    }
  }

  // Resolve to int16_t pixels (for BlockStyle fields)
  [[nodiscard]] int16_t toPixelsInt16(const float emSize, const float containerWidth = 0) const {
    const int32_t scaled = scaledValue();
    if (scaled == 0) {
      return 0;
    }
    const int32_t emPixels = static_cast<int32_t>(emSize);
    const int32_t containerPixels = static_cast<int32_t>(containerWidth);

    switch (unit()) {
      case CssUnit::Em:
      case CssUnit::Rem: {
        return clampToInt16((scaled * emPixels) / kScale);
      }
      case CssUnit::Points: {
        return clampToInt16((scaled * 133) / (kScale * 100));
      }
      case CssUnit::Percent: {
        return clampToInt16((scaled * containerPixels) / (kScale * 100));
      }
      default:
        return clampToInt16(scaled / kScale);
    }
  }

 private:
  static constexpr uint32_t kUnitBits = 3;
  static constexpr uint32_t kUnitMask = (1u << kUnitBits) - 1u;
  // Decimal scale preserves 0.01-style CSS inputs exactly and also keeps
  // common 3-4 decimal values stable without reintroducing float drift.
  static constexpr int32_t kScale = 10000;
  static constexpr float kInvScale = 1.0f / static_cast<float>(kScale);
  static constexpr uint32_t kPayloadBits = 32u - kUnitBits;
  static constexpr uint32_t kPayloadMask = ~kUnitMask;
  static constexpr uint32_t kPayloadSignBit = 1u << (kPayloadBits - 1u);
  static constexpr uint32_t kSignExtendMask = ~((1u << kPayloadBits) - 1u);
  static constexpr int64_t kMinScaled = -(int64_t{1} << (kPayloadBits - 1u));
  static constexpr int64_t kMaxScaled = (int64_t{1} << (kPayloadBits - 1u)) - 1;

  static uint32_t pack(const float v, const CssUnit u) {
    const int64_t scaled = static_cast<int64_t>(std::llround(static_cast<double>(v) * static_cast<double>(kScale)));
    return packScaled(static_cast<int32_t>(std::clamp(scaled, kMinScaled, kMaxScaled)), u);
  }

  static uint32_t packScaled(const int32_t scaled, const CssUnit u) {
    const uint32_t payload = (static_cast<uint32_t>(scaled) << kUnitBits) & kPayloadMask;
    return payload | (static_cast<uint32_t>(u) & kUnitMask);
  }

  static int32_t unpackScaled(const uint32_t raw) {
    // The payload is stored as a signed 29-bit integer, so recover the sign by
    // extending bit 28 after shifting the unit tag away.
    uint32_t payload = raw >> kUnitBits;
    if ((payload & kPayloadSignBit) != 0) {
      payload |= kSignExtendMask;
    }
    return static_cast<int32_t>(payload);
  }

  static CssUnit unpackUnit(const uint32_t raw) { return static_cast<CssUnit>(raw & kUnitMask); }

  static int16_t clampToInt16(const int32_t value) {
    return static_cast<int16_t>(std::clamp(value, static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
                                           static_cast<int32_t>(std::numeric_limits<int16_t>::max())));
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

// Vertical alignment options for inline elements (e.g. superscript/subscript)
enum class CssVerticalAlign : uint8_t { Baseline = 0, Super = 1, Sub = 2 };

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint16_t textAlign : 1;
  uint16_t fontStyle : 1;
  uint16_t fontWeight : 1;
  uint16_t textDecoration : 1;
  uint16_t textIndent : 1;
  uint16_t marginTop : 1;
  uint16_t marginBottom : 1;
  uint16_t marginLeft : 1;
  uint16_t marginRight : 1;
  uint16_t paddingTop : 1;
  uint16_t paddingBottom : 1;
  uint16_t paddingLeft : 1;
  uint16_t paddingRight : 1;
  uint16_t imageHeight : 1;
  uint16_t imageWidth : 1;
  uint16_t display : 1;
  uint16_t direction : 1;
  uint16_t verticalAlign : 1;

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
        direction(0),
        verticalAlign(0) {}

  [[nodiscard]] bool anySet() const {
    return textAlign || fontStyle || fontWeight || textDecoration || textIndent || marginTop || marginBottom ||
           marginLeft || marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight || imageHeight ||
           imageWidth || display || direction || verticalAlign;
  }

  void clearAll() {
    textAlign = fontStyle = fontWeight = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = direction = verticalAlign = 0;
  }
};

// Cache serializes defined flags as uint32_t with bit indices 0..17.
static_assert(sizeof(CssPropertyFlags) <= sizeof(uint32_t),
              "CssPropertyFlags exceeds 32 bits; update cache read/write in CssParser.cpp");

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
struct CssStyle {
  CssTextAlign textAlign = CssTextAlign::Left;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration textDecoration = CssTextDecoration::None;
  CssTextDirection direction = CssTextDirection::Ltr;

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
  CssDisplay display = CssDisplay::Block;                       // display property (Block or None)
  CssVerticalAlign verticalAlign = CssVerticalAlign::Baseline;  // vertical-align (super/sub positioning)

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
    if (base.hasDirection()) {
      direction = base.direction;
      defined.direction = 1;
    }
    if (base.hasVerticalAlign()) {
      verticalAlign = base.verticalAlign;
      defined.verticalAlign = 1;
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
  [[nodiscard]] bool hasDirection() const { return defined.direction; }
  [[nodiscard]] bool hasVerticalAlign() const { return defined.verticalAlign; }

  void reset() {
    textAlign = CssTextAlign::Left;
    fontStyle = CssFontStyle::Normal;
    fontWeight = CssFontWeight::Normal;
    textDecoration = CssTextDecoration::None;
    direction = CssTextDirection::Ltr;
    textIndent = CssLength{};
    marginTop = marginBottom = marginLeft = marginRight = CssLength{};
    paddingTop = paddingBottom = paddingLeft = paddingRight = CssLength{};
    imageHeight = imageWidth = CssLength{};
    display = CssDisplay::Block;
    verticalAlign = CssVerticalAlign::Baseline;
    defined.clearAll();
  }
};

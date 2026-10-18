#pragma once

#include <cstdint>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings. Inherit is not an alignment: it is
// the marker for an explicit `inherit`, which must beat a value set lower in the cascade and then
// leave the element to its parent's alignment (CssStyle::hasTextAlign reports it as unset).
// Based on the `inherit` patch Leopoldo Pla Sempere (@lpla) posted on crosspoint-reader PR #3891.
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4, Inherit = 5 };
// Auto is not a length: it is the marker for an explicit `width: auto` / `height: auto`, which
// must beat a length set earlier in the cascade rather than be ignored as unparseable. Only
// imageWidth/imageHeight carry it (see CssStyle::hasImageWidth/hasImageHeight, which report the
// property as unset when it is present); toPixels() never sees it.
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4, Auto = 5 };

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

// Font style options matching CSS font-style property. Inherit: see CssTextAlign.
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1, Inherit = 2 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold. Inherit: see CssTextAlign.
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1, Inherit = 2 };

// Text decoration options
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1, LineThrough = 2, UnderlineLineThrough = 3 };

// Display options. Block and Inline only matter where the tag does not already decide it: a
// <span> styled display:block is laid out as a block, one styled display:inline is not.
enum class CssDisplay : uint8_t { Block = 0, None = 1, Inline = 2 };

// Vertical align options relevant for inline text positioning
enum class CssVerticalAlign : uint8_t { Baseline = 0, Super = 1, Sub = 2 };

// Float options — used to detect inline images beside paragraph text
enum class CssFloat : uint8_t { None = 0, Left = 1, Right = 2 };

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
  uint32_t verticalAlign : 1;
  uint32_t listStyleNone : 1;
  uint32_t pageBreakBefore : 1;
  uint32_t pageBreakAfter : 1;
  uint32_t lineHeight : 1;
  uint32_t fontSizeMultiplier : 1;
  uint32_t cssFloat : 1;
  uint32_t smallCaps : 1;
  // Invisibility. Three bits, not one: they come from three properties that cascade
  // independently, and a shared bit would let a later `visibility: visible` cancel an
  // `opacity: 0` (or vice versa) purely by declaration order.
  uint32_t colorTransparent : 1;
  uint32_t opacityZero : 1;
  uint32_t visibilityHidden : 1;
  uint32_t fontFamily : 1;

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
        verticalAlign(0),
        listStyleNone(0),
        pageBreakBefore(0),
        pageBreakAfter(0),
        lineHeight(0),
        fontSizeMultiplier(0),
        cssFloat(0),
        smallCaps(0),
        colorTransparent(0),
        opacityZero(0),
        visibilityHidden(0),
        fontFamily(0) {}

  [[nodiscard]] bool anySet() const {
    return textAlign || fontStyle || fontWeight || textDecoration || textIndent || marginTop || marginBottom ||
           marginLeft || marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight || imageHeight ||
           imageWidth || display || verticalAlign || listStyleNone || pageBreakBefore || pageBreakAfter || lineHeight ||
           fontSizeMultiplier || cssFloat || smallCaps || colorTransparent || opacityZero || visibilityHidden ||
           fontFamily;
  }

  void clearAll() {
    textAlign = fontStyle = fontWeight = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = verticalAlign = 0;
    listStyleNone = pageBreakBefore = pageBreakAfter = lineHeight = fontSizeMultiplier = cssFloat = 0;
    smallCaps = colorTransparent = opacityZero = visibilityHidden = fontFamily = 0;
  }
};

// CssStyle::fontFamily: the identity of a font-family list's first family (CssParser::fontFamilyHash, whose low two
// bits are always 0; 0 when it is a generic family or a keyword), or-ed with the list's generic serif or sans-serif
// in those two bits, the family Publisher Fonts falls back to when the book does not have the named one.
namespace CssFontFamily {
constexpr uint32_t kGenericMask = 3;
constexpr uint32_t kSerif = 1;
constexpr uint32_t kSansSerif = 2;
constexpr uint32_t identity(const uint32_t family) { return family & ~kGenericMask; }
}  // namespace CssFontFamily

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
  CssDisplay display = CssDisplay::Block;                       // display property (Block, Inline or None)
  CssVerticalAlign verticalAlign = CssVerticalAlign::Baseline;  // vertical-align for inline elements

  bool listStyleNone = false;         // true when list-style-type: none / list-style: none
  bool pageBreakBefore = false;       // true when page-break-before: always (or break-before: page)
  bool pageBreakAfter = false;        // true when page-break-after: always (or break-after: page)
  float lineHeightMultiplier = 1.0f;  // normalised line-height multiplier (relative to default y_advance)
  float fontSizeMultiplier = 1.0f;    // font-size multiplier relative to body em size
  // Part of font-size, set with it: true for rem, px, pt and the absolute keywords, whose size is
  // the body's times the multiplier; false for em, % and smaller/larger, which scale the parent's.
  bool fontSizeRootRelative = false;
  CssFloat cssFloat = CssFloat::None;  // float: left/right — signals inline image context
  bool smallCaps = false;              // font-variant: small-caps
  // Text made invisible while still in the flow. A PDF-to-EPUB conversion emits every OCR word
  // as its own absolutely positioned div with `color: transparent` under a full-page scan; laid
  // out as ordinary text that became eleven pages of single words per scan (Deckhand).
  // colorTransparent hides TEXT only (images are not coloured); opacityZero and visibilityHidden
  // hide the whole element, images included. `-webkit-text-fill-color` and alpha-zero colours
  // (rgba/hsla/#rrggbb00) set colorTransparent too.
  // Invariant: a DEFINED invisibility property is always true. The visible value (color: red,
  // opacity: 0.5, visibility: visible) undefines it rather than defining it false, so a rule
  // that sets only visible values still has no supported declaration and stays out of the rule
  // cache. The value bools exist for uniformity with every other property; see CssParser.
  bool colorTransparent = false;
  bool opacityZero = false;
  bool visibilityHidden = false;  // visibility: hidden | collapse
  // A font-family list as CssFontFamily describes it: its first family's identity and its generic serif or
  // sans-serif; 0 for the reader's font.
  uint32_t fontFamily = 0;

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  // Number of properties applyOver() merges: the size of its `ranks` array.
  static constexpr int kCascadeProperties = 28;

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined.
  //
  // With `ranks`, a property is taken only when `rank` is at least the rank of the rule that set
  // it so far (ranks[i], meaningful while the property is defined here), and the winner's rank is
  // recorded there. Rules can then be merged in any order and still end as if they had been
  // applied sorted by rank -- CssParser::resolveStyle ranks by specificity, then source order.
  void applyOver(const CssStyle& base, const uint32_t rank = 0, uint32_t* ranks = nullptr) {
    const auto take = [rank, ranks](const bool alreadySet, const int property) {
      if (ranks == nullptr) return true;
      if (alreadySet && rank < ranks[property]) return false;
      ranks[property] = rank;
      return true;
    };
    // Keyed on the defined bit, not has*(), for these three: an explicit `inherit` has to travel up
    // the cascade so it beats a value a lower-priority rule set, like `auto` for images below.
    if (base.defined.textAlign && take(defined.textAlign, 0)) {
      textAlign = base.textAlign;
      defined.textAlign = 1;
    }
    if (base.defined.fontStyle && take(defined.fontStyle, 1)) {
      fontStyle = base.fontStyle;
      defined.fontStyle = 1;
    }
    if (base.defined.fontWeight && take(defined.fontWeight, 2)) {
      fontWeight = base.fontWeight;
      defined.fontWeight = 1;
    }
    if (base.hasTextDecoration() && take(defined.textDecoration, 3)) {
      textDecoration = base.textDecoration;
      defined.textDecoration = 1;
    }
    if (base.hasTextIndent() && take(defined.textIndent, 4)) {
      textIndent = base.textIndent;
      defined.textIndent = 1;
    }
    if (base.hasMarginTop() && take(defined.marginTop, 5)) {
      marginTop = base.marginTop;
      defined.marginTop = 1;
    }
    if (base.hasMarginBottom() && take(defined.marginBottom, 6)) {
      marginBottom = base.marginBottom;
      defined.marginBottom = 1;
    }
    if (base.hasMarginLeft() && take(defined.marginLeft, 7)) {
      marginLeft = base.marginLeft;
      defined.marginLeft = 1;
    }
    if (base.hasMarginRight() && take(defined.marginRight, 8)) {
      marginRight = base.marginRight;
      defined.marginRight = 1;
    }
    if (base.hasPaddingTop() && take(defined.paddingTop, 9)) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.hasPaddingBottom() && take(defined.paddingBottom, 10)) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
    }
    if (base.hasPaddingLeft() && take(defined.paddingLeft, 11)) {
      paddingLeft = base.paddingLeft;
      defined.paddingLeft = 1;
    }
    if (base.hasPaddingRight() && take(defined.paddingRight, 12)) {
      paddingRight = base.paddingRight;
      defined.paddingRight = 1;
    }
    // Keyed on the defined bit, not has*(): an explicit `auto` has to travel up the cascade so it
    // can clear a length a lower-priority rule set. has*() reports it as unset, which is what the
    // layout wants but the wrong test here.
    if (base.defined.imageHeight && take(defined.imageHeight, 13)) {
      imageHeight = base.imageHeight;
      defined.imageHeight = 1;
    }
    if (base.defined.imageWidth && take(defined.imageWidth, 14)) {
      imageWidth = base.imageWidth;
      defined.imageWidth = 1;
    }
    if (base.hasDisplay() && take(defined.display, 15)) {
      display = base.display;
      defined.display = 1;
    }
    if (base.hasVerticalAlign() && take(defined.verticalAlign, 16)) {
      verticalAlign = base.verticalAlign;
      defined.verticalAlign = 1;
    }
    if (base.hasListStyleNone() && take(defined.listStyleNone, 17)) {
      listStyleNone = base.listStyleNone;
      defined.listStyleNone = 1;
    }
    if (base.hasPageBreakBefore() && take(defined.pageBreakBefore, 18)) {
      pageBreakBefore = base.pageBreakBefore;
      defined.pageBreakBefore = 1;
    }
    if (base.hasPageBreakAfter() && take(defined.pageBreakAfter, 19)) {
      pageBreakAfter = base.pageBreakAfter;
      defined.pageBreakAfter = 1;
    }
    if (base.hasLineHeight() && take(defined.lineHeight, 20)) {
      lineHeightMultiplier = base.lineHeightMultiplier;
      defined.lineHeight = 1;
    }
    if (base.hasFontSizeMultiplier() && take(defined.fontSizeMultiplier, 21)) {
      fontSizeMultiplier = base.fontSizeMultiplier;
      fontSizeRootRelative = base.fontSizeRootRelative;
      defined.fontSizeMultiplier = 1;
    }
    if (base.hasCssFloat() && take(defined.cssFloat, 22)) {
      cssFloat = base.cssFloat;
      defined.cssFloat = 1;
    }
    if (base.hasSmallCaps() && take(defined.smallCaps, 23)) {
      smallCaps = base.smallCaps;
      defined.smallCaps = 1;
    }
    if (base.hasColorTransparent() && take(defined.colorTransparent, 24)) {
      colorTransparent = base.colorTransparent;
      defined.colorTransparent = 1;
    }
    if (base.hasOpacityZero() && take(defined.opacityZero, 25)) {
      opacityZero = base.opacityZero;
      defined.opacityZero = 1;
    }
    if (base.hasVisibilityHidden() && take(defined.visibilityHidden, 26)) {
      visibilityHidden = base.visibilityHidden;
      defined.visibilityHidden = 1;
    }
    if (base.defined.fontFamily && take(defined.fontFamily, 27)) {
      fontFamily = base.fontFamily;
      defined.fontFamily = 1;
    }
  }

  // An explicit `inherit` is defined (it overrides lower rules in applyOver) but reports as unset:
  // the element takes its parent's value, which is what an unset property does in the parser.
  [[nodiscard]] bool hasTextAlign() const { return defined.textAlign && textAlign != CssTextAlign::Inherit; }
  [[nodiscard]] bool hasFontStyle() const { return defined.fontStyle && fontStyle != CssFontStyle::Inherit; }
  [[nodiscard]] bool hasFontWeight() const { return defined.fontWeight && fontWeight != CssFontWeight::Inherit; }
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
  // `auto` is recorded as a defined property (so it overrides a length from a lower-priority
  // rule via applyOver) but reports as "no explicit size", which is what it means: derive the
  // axis from the image's aspect ratio.
  [[nodiscard]] bool hasImageHeight() const { return defined.imageHeight && imageHeight.unit != CssUnit::Auto; }
  [[nodiscard]] bool hasImageWidth() const { return defined.imageWidth && imageWidth.unit != CssUnit::Auto; }
  [[nodiscard]] bool hasDisplay() const { return defined.display; }
  [[nodiscard]] bool hasVerticalAlign() const { return defined.verticalAlign; }
  [[nodiscard]] bool hasListStyleNone() const { return defined.listStyleNone; }
  [[nodiscard]] bool hasCssFloat() const { return defined.cssFloat; }
  [[nodiscard]] bool hasSmallCaps() const { return defined.smallCaps; }
  [[nodiscard]] bool hasColorTransparent() const { return defined.colorTransparent; }
  [[nodiscard]] bool hasOpacityZero() const { return defined.opacityZero; }
  [[nodiscard]] bool hasVisibilityHidden() const { return defined.visibilityHidden; }
  // The element and everything in it is invisible (images too): opacity 0, or visibility
  // hidden/collapse. Text-only invisibility (colorTransparent) is a separate question.
  [[nodiscard]] bool isElementHidden() const {
    return (defined.opacityZero && opacityZero) || (defined.visibilityHidden && visibilityHidden);
  }
  [[nodiscard]] bool isTextTransparent() const { return defined.colorTransparent && colorTransparent; }
  // Text declared too small to read: the `font-size: 1px` span a watermarking service hides in the
  // prose. Like colorTransparent a TEXT-only question (images are not sized by it), asked of the
  // element's own declared multiplier, before any baseline normalisation.
  // The line is a tenth of the body: 1.6 px of the CSS 16 px medium, 1.2 pt of 12 pt. Watermarks
  // sit at 1 px (0.0625) or 1 pt (0.083). Nothing meant to be read comes near it: the smallest
  // keyword (xx-small) is 0.6, the sup/sub default 0.5, and the renderer's own floor
  // (ParsedText::MIN_WORD_SIZE_PCT, 30 %) is three times above it. A tenth itself is left to that
  // floor, as test_font_sizes.epub pins.
  static constexpr float kInvisibleFontSizeMultiplier = 0.1f;
  [[nodiscard]] bool isFontSizeInvisible() const {
    return defined.fontSizeMultiplier && fontSizeMultiplier < kInvisibleFontSizeMultiplier;
  }
  [[nodiscard]] bool hasPageBreakBefore() const { return defined.pageBreakBefore; }
  [[nodiscard]] bool hasPageBreakAfter() const { return defined.pageBreakAfter; }
  [[nodiscard]] bool hasLineHeight() const { return defined.lineHeight; }
  [[nodiscard]] bool hasFontSizeMultiplier() const { return defined.fontSizeMultiplier; }

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
    verticalAlign = CssVerticalAlign::Baseline;
    listStyleNone = false;
    pageBreakBefore = false;
    pageBreakAfter = false;
    lineHeightMultiplier = 1.0f;
    fontSizeMultiplier = 1.0f;
    fontSizeRootRelative = false;
    cssFloat = CssFloat::None;
    smallCaps = false;
    colorTransparent = false;
    opacityZero = false;
    visibilityHidden = false;
    fontFamily = 0;
    defined.clearAll();
  }
};

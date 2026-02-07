#pragma once

#include <cstdint>

#include "Epub/css/CssStyle.h"

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  CssTextAlign alignment = CssTextAlign::Justify;

  // Spacing (in pixels)
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;     // treated same as margin for rendering
  int16_t paddingBottom = 0;  // treated same as margin for rendering
  int16_t paddingLeft = 0;    // treated same as margin for rendering
  int16_t paddingRight = 0;   // treated same as margin for rendering
  int16_t textIndent = 0;
  bool textIndentDefined = false;  // true if text-indent was explicitly set in CSS
  bool textAlignDefined = false;   // true if text-align was explicitly set in CSS

  // Combined horizontal insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }

  // Combine with another block style. Useful for parent -> child styles, where the child style should be
  // applied on top of the parent's style to get the combined style.
  BlockStyle getCombinedBlockStyle(const BlockStyle& child) const {
    BlockStyle combinedBlockStyle;

    combinedBlockStyle.marginTop = static_cast<int16_t>(child.marginTop + marginTop);
    combinedBlockStyle.marginBottom = static_cast<int16_t>(child.marginBottom + marginBottom);
    combinedBlockStyle.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
    combinedBlockStyle.marginRight = static_cast<int16_t>(child.marginRight + marginRight);

    combinedBlockStyle.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
    combinedBlockStyle.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
    combinedBlockStyle.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
    combinedBlockStyle.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
    // Text indent: use child's if defined
    if (child.textIndentDefined) {
      combinedBlockStyle.textIndent = child.textIndent;
      combinedBlockStyle.textIndentDefined = true;
    } else {
      combinedBlockStyle.textIndent = textIndent;
      combinedBlockStyle.textIndentDefined = textIndentDefined;
    }
    // Text align: use child's if defined
    if (child.textAlignDefined) {
      combinedBlockStyle.alignment = child.alignment;
      combinedBlockStyle.textAlignDefined = true;
    } else {
      combinedBlockStyle.alignment = alignment;
      combinedBlockStyle.textAlignDefined = textAlignDefined;
    }
    return combinedBlockStyle;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment) {
    BlockStyle blockStyle;
    // Resolve all CssLength values to pixels using the current font's em size
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize);
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize);
    blockStyle.marginLeft = cssStyle.marginLeft.toPixelsInt16(emSize);
    blockStyle.marginRight = cssStyle.marginRight.toPixelsInt16(emSize);

    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize);
    blockStyle.paddingLeft = cssStyle.paddingLeft.toPixelsInt16(emSize);
    blockStyle.paddingRight = cssStyle.paddingRight.toPixelsInt16(emSize);

    blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize);
    blockStyle.textIndentDefined = cssStyle.hasTextIndent();
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    // User setting overrides CSS, unless "Book's Style" alignment setting is selected
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    return blockStyle;
  }
};

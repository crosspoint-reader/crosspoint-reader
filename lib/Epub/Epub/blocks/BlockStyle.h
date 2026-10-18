#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/css/CssStyle.h"

// Vertical exclusion zone created by a left-floated inline image.
// Lines whose vertical extent overlaps [top, bottom) have their available
// width reduced by `width` pixels so text wraps correctly beside the image.
// Coordinates are relative to the page top (set provisionally at parse time,
// then corrected to the true baseline in addLineToPage).
// NOT serialised — computed at parse time, discarded after layout.
// NOT propagated through getCombinedBlockStyle — consumed by the single
// paragraph it was attached to.
struct FloatZone {
  int16_t top;     // y of image top relative to page top
  int16_t bottom;  // top + imageHeight
  int16_t width;   // imageWidth + 4 px gap
  bool isRight;    // true = image floated right (text narrows from right, no left xOffset shift)
};

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  static constexpr int kMaxFloatZones = 2;

  // Upper bound (in em) for any single side's horizontal margin or padding.
  // Some EPUBs apply huge em-based insets to chapter-opener classes; without a
  // cap, effectiveWidth collapses to 1-2 words per line and justification dumps
  // the remaining space into a single gap.
  static constexpr float MAX_HORIZONTAL_INSET_EM = 4.0f;

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
  // The indent came from margin-left on an inline span (one poem line drawn indented), and gives
  // way before the line does: see ParsedText::layoutAndExtractLines.
  bool textIndentYields = false;
  bool textAlignDefined = false;  // true if text-align was explicitly set in CSS
  // Set when this block was created by a <br> element. Used by startNewTextBlock to inject
  // a full line-height gap when the <br> block stays empty (section-break use case).
  // NOT propagated through getCombinedBlockStyle so it can't leak into sibling blocks.
  bool fromBrElement = false;
  // Parse-time guard: true once resolveBlockFont() has snapped this block to the size
  // ladder, so a second layout pass (long-block split, continuations) can't re-resolve the
  // residual multiplier. NOT serialised and NOT propagated through getCombinedBlockStyle
  // (a fresh combined style defaults to unresolved, which is correct — it precedes layout).
  bool fontResolved = false;
  // A heading: the page must not end right after it (ChapterHtmlSlimParser::keepTogetherBreak
  // keeps room for it and the first lines of what follows). Parse-time only, NOT serialised;
  // getCombinedBlockStyle takes the child's, so a wrapper's empty block merging into a heading
  // gets it and a spent heading block lends it to nothing.
  bool keepWithNext = false;
  // Heading sizing. Two strategies share these two fields:
  //  - headingFontId == 0: scale the *body* font by fontSizeMultiplier (nearest-neighbor
  //    upscale path). Used for SD fonts (only one size loaded), steps past the largest
  //    built-in size, and CSS-driven arbitrary multipliers (font-size:1.7em).
  //  - headingFontId != 0: render with that (taller, real) built-in fontId; fontSizeMultiplier
  //    is then a small residual (usually 1.0) applied on top, so glyphs stay crisp.
  // h1=1.6, h2=1.4, h3=1.2 are the default desired multipliers when no explicit CSS size.
  float fontSizeMultiplier = 1.0f;
  // 32-bit font-id hash (fontIds.h); 0 = use the body font + fontSizeMultiplier. Must be a
  // full int — a uint8_t would truncate the hash and the heading would render nothing.
  int32_t headingFontId = 0;
  int16_t firstLineExtraIndent = 0;  // extra indent on the first line only (combined with CSS text-indent)

  // Float zones: left-floated images that narrow text width on overlapping lines.
  FloatZone floatZones[kMaxFloatZones] = {};
  int8_t floatZoneCount = 0;

  // Clamp one horizontal margin/padding value into [0, maxPx]. The upper bound is
  // MAX_HORIZONTAL_INSET_EM (see above); the lower bound exists because a block is laid out
  // against the viewport, not against a parent box -- a negative inset has nothing to pull
  // back into, so it only pushes the first glyphs off the left edge of the panel.
  static int16_t clampInset(const int16_t px, const int16_t maxPx) { return px < 0 ? 0 : std::min(px, maxPx); }

  // Combined horizontal insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }

  // Combine with another block style. Useful for parent -> child styles, where the child style should be
  // applied on top of the parent's style to get the combined style.
  BlockStyle getCombinedBlockStyle(const BlockStyle& child) const {
    BlockStyle combinedBlockStyle;

    // Vertical margins between a parent and its child collapse (CSS collapsing margins):
    // the combined margin is the larger of the two, not their sum. Without this, a
    // margined block wrapping a margined child (e.g. <div> around an <h1>) double-counts
    // the gap and over-spaces. Vertical padding stays additive; for the horizontal sides
    // see below.
    combinedBlockStyle.marginTop = std::max(child.marginTop, marginTop);
    combinedBlockStyle.marginBottom = std::max(child.marginBottom, marginBottom);

    combinedBlockStyle.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
    combinedBlockStyle.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);

    // Horizontal insets are the child's alone. They used to accumulate here, which was this
    // merge's only way of getting a wrapper's margin into the block that finally holds the
    // text -- but it reaches the wrapper's FIRST child only, and it also lets a spent sibling
    // block donate its margin. Enclosing insets now arrive on every child through the parser's
    // blockInsetStack_, so summing them here as well would double them.
    combinedBlockStyle.marginLeft = child.marginLeft;
    combinedBlockStyle.marginRight = child.marginRight;
    combinedBlockStyle.paddingLeft = child.paddingLeft;
    combinedBlockStyle.paddingRight = child.paddingRight;
    // Text indent: use child's if defined
    if (child.textIndentDefined) {
      combinedBlockStyle.textIndent = child.textIndent;
      combinedBlockStyle.textIndentDefined = true;
      combinedBlockStyle.textIndentYields = child.textIndentYields;
    } else {
      combinedBlockStyle.textIndent = textIndent;
      combinedBlockStyle.textIndentDefined = textIndentDefined;
      combinedBlockStyle.textIndentYields = textIndentYields;
    }
    // Text align: use child's if defined
    if (child.textAlignDefined) {
      combinedBlockStyle.alignment = child.alignment;
      combinedBlockStyle.textAlignDefined = true;
    } else {
      combinedBlockStyle.alignment = alignment;
      combinedBlockStyle.textAlignDefined = textAlignDefined;
    }
    // fromBrElement is never propagated — it is consumed by startNewTextBlock
    // when the empty <br> block is merged with the following paragraph.
    combinedBlockStyle.fromBrElement = false;
    combinedBlockStyle.keepWithNext = child.keepWithNext;
    // fontSizeMultiplier: use child's if != 1.0, else parent's
    combinedBlockStyle.fontSizeMultiplier =
        (child.fontSizeMultiplier != 1.0f) ? child.fontSizeMultiplier : fontSizeMultiplier;
    // headingFontId: use child's if set, else parent's, so inline children of a heading
    // (e.g. <em> inside an <h2>) inherit the taller heading font.
    combinedBlockStyle.headingFontId = (child.headingFontId != 0) ? child.headingFontId : headingFontId;
    return combinedBlockStyle;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0) {
    BlockStyle blockStyle;
    const float vw = viewportWidth;
    const auto maxHorizontalInsetPx = static_cast<int16_t>(emSize * MAX_HORIZONTAL_INSET_EM);
    // Resolve all CssLength values to pixels using the current font's em size and viewport width
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, vw);
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, vw);
    blockStyle.marginLeft = clampInset(cssStyle.marginLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.marginRight = clampInset(cssStyle.marginRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, vw);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, vw);
    blockStyle.paddingLeft = clampInset(cssStyle.paddingLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    blockStyle.paddingRight = clampInset(cssStyle.paddingRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

    // For textIndent: if it's a percentage we can't resolve (no viewport width),
    // leave textIndentDefined=false so applyParagraphIndent() applies a pixel fallback
    if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
      blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
      blockStyle.textIndentDefined = true;
    }
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    // User setting overrides CSS, unless "Book's Style" alignment setting is selected
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    if (cssStyle.hasFontSizeMultiplier()) {
      blockStyle.fontSizeMultiplier = cssStyle.fontSizeMultiplier;
    }
    return blockStyle;
  }
};

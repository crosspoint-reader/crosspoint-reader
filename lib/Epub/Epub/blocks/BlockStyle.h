#pragma once

#include <cstdint>

/**
 * BlockStyle - Block-level CSS properties for paragraphs
 *
 * Used to track margin/padding spacing and text indentation for block elements.
 * Padding is treated similarly to margins for rendering purposes.
 */
struct BlockStyle {
  int8_t marginTop = 0;      // 0-2 lines
  int8_t marginBottom = 0;   // 0-2 lines
  int8_t paddingTop = 0;     // 0-2 lines (treated same as margin)
  int8_t paddingBottom = 0;  // 0-2 lines (treated same as margin)
  int16_t textIndent = 0;    // pixels
};

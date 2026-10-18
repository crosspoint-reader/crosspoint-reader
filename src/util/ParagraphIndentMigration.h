#pragma once

#include <cstdint>

constexpr uint8_t migrateParagraphIndentSpaces(const bool hasSavedWidth, const int savedWidth,
                                               const bool extraParagraphSpacing) {
  if (!hasSavedWidth) return extraParagraphSpacing ? 0 : 2;
  if (savedWidth < 0) return 0;
  return savedWidth > 5 ? 5 : static_cast<uint8_t>(savedWidth);
}

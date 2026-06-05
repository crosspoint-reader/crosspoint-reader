#include "CjkLayout.h"

bool isCJKLeadingPunctuation(uint32_t cp) {
  for (size_t i = 0; i < CJK_LEADING_PUNCTUATION_COUNT; ++i) {
    if (CJK_LEADING_PUNCTUATION[i] == cp) return true;
  }
  return false;
}

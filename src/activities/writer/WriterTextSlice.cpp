#include "WriterTextSlice.h"

#include <algorithm>

std::string WriterTextSlice::slice(const std::string& text, const size_t startOffset, const size_t endOffset) {
  const size_t safeStart = std::min(startOffset, text.size());
  const size_t safeEnd = std::min(std::max(endOffset, safeStart), text.size());
  return text.substr(safeStart, safeEnd - safeStart);
}

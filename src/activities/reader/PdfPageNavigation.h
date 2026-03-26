#pragma once

#include <cstdint>

struct PdfPageNavigationState {
  uint32_t page = 0;
  uint16_t slice = 0;
};

inline bool pdfPageTurnForward(PdfPageNavigationState& state, uint32_t totalPages, uint16_t currentSliceCount) {
  if (currentSliceCount > 0 && state.slice + 1 < currentSliceCount) {
    ++state.slice;
    return true;
  }
  if (state.page + 1 < totalPages) {
    ++state.page;
    state.slice = 0;
    return true;
  }
  return false;
}

inline bool pdfPageTurnBackward(PdfPageNavigationState& state, uint16_t previousSliceCount) {
  if (state.slice > 0) {
    --state.slice;
    return true;
  }
  if (state.page == 0) {
    return false;
  }
  --state.page;
  state.slice = previousSliceCount > 0 ? static_cast<uint16_t>(previousSliceCount - 1) : 0;
  return true;
}

#include "WriterViewport.h"

#include <algorithm>

void WriterViewport::ensureCursorVisible(State& state,
                                          int cursorLine,
                                          int maxVisibleLines,
                                          int scrollStep,
                                          int totalLines) {
  const int visibleLines = std::max(1, maxVisibleLines);
  const int documentLines = std::max(0, totalLines);
  const int maxTopLine = std::max(0, documentLines - visibleLines);
  const int step = std::max(1, scrollStep);

  while (cursorLine < state.topLine) {
    state.topLine -= step;
  }

  while (cursorLine >= state.topLine + visibleLines) {
    state.topLine += step;
  }

  state.topLine = std::clamp(state.topLine, 0, maxTopLine);
}

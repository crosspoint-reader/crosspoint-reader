#include <cstdlib>
#include <iostream>

#include "src/activities/writer/WriterViewport.h"

namespace {

void expectEqual(int actual, int expected, const char* testName) {
  if (actual == expected) {
    return;
  }

  std::cerr << "FAILED: " << testName << " expected " << expected << " got " << actual << "\n";
  std::exit(1);
}

// ensureCursorVisible(State& state, int cursorLine, int maxVisibleLines, int scrollStep, int totalLines)
void scrollsUpByConfiguredStepWhenCursorMovesAboveViewport() {
  WriterViewport::State state{10};
  WriterViewport::ensureCursorVisible(state, 8, 10, 4, 100);
  expectEqual(state.topLine, 6, "scrollsUpByConfiguredStepWhenCursorMovesAboveViewport");
}

void scrollsDownByConfiguredStepWhenCursorMovesBelowViewport() {
  WriterViewport::State state{10};
  WriterViewport::ensureCursorVisible(state, 21, 10, 4, 100);
  expectEqual(state.topLine, 14, "scrollsDownByConfiguredStepWhenCursorMovesBelowViewport");
}

void leavesViewportUnchangedWhenCursorAlreadyVisible() {
  WriterViewport::State state{10};
  WriterViewport::ensureCursorVisible(state, 15, 10, 4, 100);
  expectEqual(state.topLine, 10, "leavesViewportUnchangedWhenCursorAlreadyVisible");
}

void clampsViewportAtTop() {
  WriterViewport::State state{1};
  WriterViewport::ensureCursorVisible(state, 0, 10, 4, 100);
  expectEqual(state.topLine, 0, "clampsViewportAtTop");
}

void treatsNonPositiveScrollStepAsSingleLineMovement() {
  WriterViewport::State state{10};
  WriterViewport::ensureCursorVisible(state, 8, 10, 0, 100);
  expectEqual(state.topLine, 8, "treatsNonPositiveScrollStepAsSingleLineMovement");
}

void clampsViewportAtBottomNearEndOfDocument() {
  WriterViewport::State state{80};
  WriterViewport::ensureCursorVisible(state, 99, 10, 4, 100);
  expectEqual(state.topLine, 90, "clampsViewportAtBottomNearEndOfDocument");
}

void keepsShortDocumentsPinnedToTop() {
  WriterViewport::State state{3};
  WriterViewport::ensureCursorVisible(state, 2, 10, 4, 4);
  expectEqual(state.topLine, 0, "keepsShortDocumentsPinnedToTop");
}

}  // namespace

int main() {
  scrollsUpByConfiguredStepWhenCursorMovesAboveViewport();
  scrollsDownByConfiguredStepWhenCursorMovesBelowViewport();
  leavesViewportUnchangedWhenCursorAlreadyVisible();
  clampsViewportAtTop();
  treatsNonPositiveScrollStepAsSingleLineMovement();
  clampsViewportAtBottomNearEndOfDocument();
  keepsShortDocumentsPinnedToTop();
  std::cout << "WriterViewportTest passed\n";
  return 0;
}

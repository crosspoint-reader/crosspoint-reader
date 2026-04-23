#pragma once

class WriterViewport {
 public:
  struct State {
    int topLine = 0;
  };

  static void ensureCursorVisible(State& state, int cursorLine, int maxVisibleLines, int scrollStep, int totalLines);
};

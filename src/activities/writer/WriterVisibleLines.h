#pragma once

#include <string>
#include <vector>

class WriterVisibleLines {
 public:
  static void appendWrappedLines(std::vector<std::string>& visibleLines,
                                 const std::vector<std::string>& wrappedLines,
                                 int maxVisibleLines);
};

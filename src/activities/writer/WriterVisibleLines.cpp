#include "WriterVisibleLines.h"

void WriterVisibleLines::appendWrappedLines(std::vector<std::string>& visibleLines,
                                            const std::vector<std::string>& wrappedLines,
                                            int maxVisibleLines) {
  for (const auto& line : wrappedLines) {
    visibleLines.push_back(line);
  }

  while (static_cast<int>(visibleLines.size()) > maxVisibleLines) {
    visibleLines.erase(visibleLines.begin());
  }
}

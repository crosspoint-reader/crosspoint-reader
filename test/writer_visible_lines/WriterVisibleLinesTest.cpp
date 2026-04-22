#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "src/activities/writer/WriterVisibleLines.h"

namespace {

void expectEqual(const std::vector<std::string>& actual,
                 const std::vector<std::string>& expected,
                 const char* testName) {
  if (actual == expected) {
    return;
  }

  std::cerr << "FAILED: " << testName << "\nExpected:\n";
  for (const auto& line : expected) {
    std::cerr << "  " << line << "\n";
  }
  std::cerr << "Actual:\n";
  for (const auto& line : actual) {
    std::cerr << "  " << line << "\n";
  }
  std::exit(1);
}

void keepsTailOfOverlongParagraph() {
  std::vector<std::string> visibleLines;
  WriterVisibleLines::appendWrappedLines(visibleLines, {"line 1", "line 2", "line 3", "line 4", "line 5"}, 3);

  expectEqual(visibleLines, {"line 3", "line 4", "line 5"}, "keepsTailOfOverlongParagraph");
}

}  // namespace

int main() {
  keepsTailOfOverlongParagraph();
  std::cout << "WriterVisibleLinesTest passed\n";
  return 0;
}

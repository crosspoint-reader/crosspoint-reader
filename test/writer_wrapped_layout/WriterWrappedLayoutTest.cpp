#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "src/activities/writer/WriterWrappedLayout.h"

namespace {

void fail(const char* testName, const std::string& message) {
  std::cerr << "FAILED: " << testName << "\n" << message << "\n";
  std::exit(1);
}

void expectEqual(size_t actual, size_t expected, const char* testName, const char* field) {
  if (actual == expected) {
    return;
  }

  fail(testName, std::string(field) + " expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

void expectEqual(const std::string& actual, const std::string& expected, const char* testName, const char* field) {
  if (actual == expected) {
    return;
  }

  fail(testName, std::string(field) + " expected [" + expected + "], got [" + actual + "]");
}

void expectTrue(bool condition, const char* testName, const char* message) {
  if (!condition) {
    fail(testName, message);
  }
}

void preservesBlankLines() {
  const auto lines = WriterWrappedLayout::wrap("alpha\n\nbeta", 10);

  expectEqual(lines.size(), 3, "preservesBlankLines", "line count");
  expectEqual(lines[0].text, "alpha", "preservesBlankLines", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "preservesBlankLines", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 5, "preservesBlankLines", "line 0 endOffset");

  expectEqual(lines[1].text, "", "preservesBlankLines", "line 1 text");
  expectEqual(lines[1].startOffset, 6, "preservesBlankLines", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "preservesBlankLines", "line 1 endOffset");

  expectEqual(lines[2].text, "beta", "preservesBlankLines", "line 2 text");
  expectEqual(lines[2].startOffset, 7, "preservesBlankLines", "line 2 startOffset");
  expectEqual(lines[2].endOffset, 11, "preservesBlankLines", "line 2 endOffset");
}

void keepsOffsetsIncreasingAcrossWrappedOutput() {
  const std::string text = "wrap these words";
  const auto lines = WriterWrappedLayout::wrap(text, 6);

  expectEqual(lines.size(), 3, "keepsOffsetsIncreasingAcrossWrappedOutput", "line count");
  expectEqual(lines[0].text, "wrap", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 0 text");
  expectEqual(lines[1].text, "these", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 1 text");
  expectEqual(lines[2].text, "words", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 2 text");

  expectTrue(lines[0].startOffset < lines[0].endOffset,
             "keepsOffsetsIncreasingAcrossWrappedOutput",
             "first line should consume source bytes");
  expectTrue(lines[0].endOffset <= lines[1].startOffset,
             "keepsOffsetsIncreasingAcrossWrappedOutput",
             "line 1 should start at or after line 0 end");
  expectTrue(lines[1].endOffset <= lines[2].startOffset,
             "keepsOffsetsIncreasingAcrossWrappedOutput",
             "line 2 should start at or after line 1 end");
  expectTrue(lines[2].endOffset <= text.size(),
             "keepsOffsetsIncreasingAcrossWrappedOutput",
             "final endOffset should stay within source text");
}

void avoidsSplittingUtf8SequencesMidByte() {
  const std::string text = "\xC3\xA9" "\xC3\xA9" "\xC3\xA9";
  const auto lines = WriterWrappedLayout::wrap(text, 2);

  expectEqual(lines.size(), 2, "avoidsSplittingUtf8SequencesMidByte", "line count");
  expectEqual(lines[0].text, "\xC3\xA9" "\xC3\xA9", "avoidsSplittingUtf8SequencesMidByte", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "avoidsSplittingUtf8SequencesMidByte", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "avoidsSplittingUtf8SequencesMidByte", "line 0 endOffset");
  expectEqual(lines[1].text, "\xC3\xA9", "avoidsSplittingUtf8SequencesMidByte", "line 1 text");
  expectEqual(lines[1].startOffset, 4, "avoidsSplittingUtf8SequencesMidByte", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "avoidsSplittingUtf8SequencesMidByte", "line 1 endOffset");
}

}  // namespace

int main() {
  preservesBlankLines();
  keepsOffsetsIncreasingAcrossWrappedOutput();
  avoidsSplittingUtf8SequencesMidByte();
  std::cout << "WriterWrappedLayoutTest passed\n";
  return 0;
}

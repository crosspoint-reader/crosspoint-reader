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

int fixedWidthMeasure(const std::string& text) {
  int width = 0;
  for (const unsigned char ch : text) {
    if ((ch & 0xC0) != 0x80) {
      ++width;
    }
  }
  return width;
}

std::vector<WriterWrappedLayout::Line> wrapWithFixedWidth(const std::string& text, int maxWidth) {
  return WriterWrappedLayout::wrap(text, maxWidth, fixedWidthMeasure);
}

int weightedMeasure(const std::string& text) {
  int width = 0;
  for (const char ch : text) {
    if (ch == 'W') {
      width += 4;
    } else if (ch == 'i') {
      width += 1;
    } else {
      width += 2;
    }
  }
  return width;
}

std::vector<WriterWrappedLayout::Line> wrapWithWeightedWidth(const std::string& text, int maxWidth) {
  return WriterWrappedLayout::wrap(text, maxWidth, weightedMeasure);
}

void wrapsUsingMeasuredWidthNotCharacterCount() {
  const auto lines = wrapWithWeightedWidth("iiii W", 6);

  expectEqual(lines.size(), 2, "wrapsUsingMeasuredWidthNotCharacterCount", "line count");
  expectEqual(lines[0].text, "iiii", "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 endOffset");
  expectEqual(lines[1].text, "W", "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 text");
  expectEqual(lines[1].startOffset, 5, "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 endOffset");
}

void hardWrapsLongWordsWithoutEllipsis() {
  const auto lines = wrapWithFixedWidth("abcdef", 2);

  expectEqual(lines.size(), 3, "hardWrapsLongWordsWithoutEllipsis", "line count");
  expectEqual(lines[0].text, "ab", "hardWrapsLongWordsWithoutEllipsis", "line 0 text");
  expectEqual(lines[1].text, "cd", "hardWrapsLongWordsWithoutEllipsis", "line 1 text");
  expectEqual(lines[2].text, "ef", "hardWrapsLongWordsWithoutEllipsis", "line 2 text");
}

void emitsOversizedCodepointAsSingleLine() {
  const auto lines = wrapWithWeightedWidth("Wii", 2);

  expectEqual(lines.size(), 2, "emitsOversizedCodepointAsSingleLine", "line count");
  expectEqual(lines[0].text, "W", "emitsOversizedCodepointAsSingleLine", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "emitsOversizedCodepointAsSingleLine", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 1, "emitsOversizedCodepointAsSingleLine", "line 0 endOffset");
  expectEqual(lines[1].text, "ii", "emitsOversizedCodepointAsSingleLine", "line 1 text");
}

void keepsUtf8CodepointsWholeWhenMeasured() {
  const std::string text =
      "\xC3\xA9"
      "\xC3\xA9"
      "\xC3\xA9";
  const auto lines = wrapWithFixedWidth(text, 2);

  expectEqual(lines.size(), 2, "keepsUtf8CodepointsWholeWhenMeasured", "line count");
  expectEqual(lines[0].text,
              "\xC3\xA9"
              "\xC3\xA9",
              "keepsUtf8CodepointsWholeWhenMeasured", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "keepsUtf8CodepointsWholeWhenMeasured", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "keepsUtf8CodepointsWholeWhenMeasured", "line 0 endOffset");
  expectEqual(lines[1].text, "\xC3\xA9", "keepsUtf8CodepointsWholeWhenMeasured", "line 1 text");
  expectEqual(lines[1].startOffset, 4, "keepsUtf8CodepointsWholeWhenMeasured", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "keepsUtf8CodepointsWholeWhenMeasured", "line 1 endOffset");
}

void preservesBlankLines() {
  const auto lines = wrapWithFixedWidth("alpha\n\nbeta", 10);

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
  const auto lines = wrapWithFixedWidth(text, 6);

  expectEqual(lines.size(), 3, "keepsOffsetsIncreasingAcrossWrappedOutput", "line count");
  expectEqual(lines[0].text, "wrap", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 0 text");
  expectEqual(lines[1].text, "these", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 1 text");
  expectEqual(lines[2].text, "words", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 2 text");

  expectTrue(lines[0].startOffset < lines[0].endOffset, "keepsOffsetsIncreasingAcrossWrappedOutput",
             "first line should consume source bytes");
  expectTrue(lines[0].endOffset <= lines[1].startOffset, "keepsOffsetsIncreasingAcrossWrappedOutput",
             "line 1 should start at or after line 0 end");
  expectTrue(lines[1].endOffset <= lines[2].startOffset, "keepsOffsetsIncreasingAcrossWrappedOutput",
             "line 2 should start at or after line 1 end");
  expectTrue(lines[2].endOffset <= text.size(), "keepsOffsetsIncreasingAcrossWrappedOutput",
             "final endOffset should stay within source text");
}

void avoidsSplittingUtf8SequencesMidByte() {
  const std::string text =
      "\xC3\xA9"
      "\xC3\xA9"
      "\xC3\xA9";
  const auto lines = wrapWithFixedWidth(text, 2);

  expectEqual(lines.size(), 2, "avoidsSplittingUtf8SequencesMidByte", "line count");
  expectEqual(lines[0].text,
              "\xC3\xA9"
              "\xC3\xA9",
              "avoidsSplittingUtf8SequencesMidByte", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "avoidsSplittingUtf8SequencesMidByte", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "avoidsSplittingUtf8SequencesMidByte", "line 0 endOffset");
  expectEqual(lines[1].text, "\xC3\xA9", "avoidsSplittingUtf8SequencesMidByte", "line 1 text");
  expectEqual(lines[1].startOffset, 4, "avoidsSplittingUtf8SequencesMidByte", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "avoidsSplittingUtf8SequencesMidByte", "line 1 endOffset");
}

}  // namespace

int main() {
  wrapsUsingMeasuredWidthNotCharacterCount();
  hardWrapsLongWordsWithoutEllipsis();
  emitsOversizedCodepointAsSingleLine();
  keepsUtf8CodepointsWholeWhenMeasured();
  preservesBlankLines();
  keepsOffsetsIncreasingAcrossWrappedOutput();
  avoidsSplittingUtf8SequencesMidByte();
  std::cout << "WriterWrappedLayoutTest passed\n";
  return 0;
}

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "src/activities/writer/WriterWrappedLayout.h"

namespace {

static_assert(sizeof(WriterWrappedLayout::Line) == sizeof(size_t) * 2,
              "wrapped lines should only store source offsets, not copied text");

int measureCallCount = 0;

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

void expectGreaterOrEqual(size_t actual, size_t minimum, const char* testName, const char* field) {
  if (actual >= minimum) {
    return;
  }

  fail(testName,
       std::string(field) + " expected at least " + std::to_string(minimum) + ", got " + std::to_string(actual));
}

int fixedWidthMeasure(void*, const std::string& text) {
  ++measureCallCount;
  int width = 0;
  for (const unsigned char ch : text) {
    if ((ch & 0xC0) != 0x80) {
      ++width;
    }
  }
  return width;
}

std::vector<WriterWrappedLayout::Line> wrapWithFixedWidth(const std::string& text, int maxWidth) {
  return WriterWrappedLayout::wrap(text, maxWidth, WriterWrappedLayout::MeasureText{nullptr, fixedWidthMeasure});
}

std::string lineText(const std::string& text, const WriterWrappedLayout::Line& line) {
  return text.substr(line.startOffset, line.endOffset - line.startOffset);
}

int weightedMeasure(void*, const std::string& text) {
  ++measureCallCount;
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
  return WriterWrappedLayout::wrap(text, maxWidth, WriterWrappedLayout::MeasureText{nullptr, weightedMeasure});
}

void wrapsUsingMeasuredWidthNotCharacterCount() {
  measureCallCount = 0;
  const std::string text = "iiii W";
  const auto lines = wrapWithWeightedWidth(text, 6);

  expectEqual(lines.size(), 2, "wrapsUsingMeasuredWidthNotCharacterCount", "line count");
  expectEqual(lineText(text, lines[0]), "iiii", "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 endOffset");
  expectEqual(lineText(text, lines[1]), "W", "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 text");
  expectEqual(lines[1].startOffset, 5, "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 endOffset");
}

void triesWholeWordsBeforeHardWrapping() {
  measureCallCount = 0;
  const std::string text = "hello extraordinary";
  const auto lines = wrapWithFixedWidth(text, 13);

  expectEqual(lines.size(), 2, "triesWholeWordsBeforeHardWrapping", "line count");
  expectEqual(lineText(text, lines[0]), "hello", "triesWholeWordsBeforeHardWrapping", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "triesWholeWordsBeforeHardWrapping", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 5, "triesWholeWordsBeforeHardWrapping", "line 0 endOffset");
  expectEqual(lineText(text, lines[1]), "extraordinary", "triesWholeWordsBeforeHardWrapping", "line 1 text");
  expectEqual(lines[1].startOffset, 6, "triesWholeWordsBeforeHardWrapping", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 19, "triesWholeWordsBeforeHardWrapping", "line 1 endOffset");
  expectTrue(measureCallCount <= 5, "triesWholeWordsBeforeHardWrapping",
             "word-first wrapping should measure whole-word candidates instead of growing codepoint substrings");
}

void hardWrapsLongWordsWithoutEllipsis() {
  measureCallCount = 0;
  const std::string text = "abcdef";
  const auto lines = wrapWithFixedWidth(text, 2);

  expectEqual(lines.size(), 3, "hardWrapsLongWordsWithoutEllipsis", "line count");
  expectEqual(lineText(text, lines[0]), "ab", "hardWrapsLongWordsWithoutEllipsis", "line 0 text");
  expectEqual(lineText(text, lines[1]), "cd", "hardWrapsLongWordsWithoutEllipsis", "line 1 text");
  expectEqual(lineText(text, lines[2]), "ef", "hardWrapsLongWordsWithoutEllipsis", "line 2 text");
}

void emitsOversizedCodepointAsSingleLine() {
  measureCallCount = 0;
  const std::string text = "Wii";
  const auto lines = wrapWithWeightedWidth(text, 2);

  expectEqual(lines.size(), 2, "emitsOversizedCodepointAsSingleLine", "line count");
  expectEqual(lineText(text, lines[0]), "W", "emitsOversizedCodepointAsSingleLine", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "emitsOversizedCodepointAsSingleLine", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 1, "emitsOversizedCodepointAsSingleLine", "line 0 endOffset");
  expectEqual(lineText(text, lines[1]), "ii", "emitsOversizedCodepointAsSingleLine", "line 1 text");
}

void keepsUtf8CodepointsWholeWhenMeasured() {
  measureCallCount = 0;
  const std::string text =
      "\xC3\xA9"
      "\xC3\xA9"
      "\xC3\xA9";
  const auto lines = wrapWithFixedWidth(text, 2);

  expectEqual(lines.size(), 2, "keepsUtf8CodepointsWholeWhenMeasured", "line count");
  expectEqual(lineText(text, lines[0]),
              "\xC3\xA9"
              "\xC3\xA9",
              "keepsUtf8CodepointsWholeWhenMeasured", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "keepsUtf8CodepointsWholeWhenMeasured", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "keepsUtf8CodepointsWholeWhenMeasured", "line 0 endOffset");
  expectEqual(lineText(text, lines[1]), "\xC3\xA9", "keepsUtf8CodepointsWholeWhenMeasured", "line 1 text");
  expectEqual(lines[1].startOffset, 4, "keepsUtf8CodepointsWholeWhenMeasured", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "keepsUtf8CodepointsWholeWhenMeasured", "line 1 endOffset");
}

void preservesBlankLines() {
  measureCallCount = 0;
  const std::string text = "alpha\n\nbeta";
  const auto lines = wrapWithFixedWidth(text, 10);

  expectEqual(lines.size(), 3, "preservesBlankLines", "line count");
  expectEqual(lineText(text, lines[0]), "alpha", "preservesBlankLines", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "preservesBlankLines", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 5, "preservesBlankLines", "line 0 endOffset");

  expectEqual(lineText(text, lines[1]), "", "preservesBlankLines", "line 1 text");
  expectEqual(lines[1].startOffset, 6, "preservesBlankLines", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "preservesBlankLines", "line 1 endOffset");

  expectEqual(lineText(text, lines[2]), "beta", "preservesBlankLines", "line 2 text");
  expectEqual(lines[2].startOffset, 7, "preservesBlankLines", "line 2 startOffset");
  expectEqual(lines[2].endOffset, 11, "preservesBlankLines", "line 2 endOffset");
}

void keepsOffsetsIncreasingAcrossWrappedOutput() {
  measureCallCount = 0;
  const std::string text = "wrap these words";
  const auto lines = wrapWithFixedWidth(text, 6);

  expectEqual(lines.size(), 3, "keepsOffsetsIncreasingAcrossWrappedOutput", "line count");
  expectEqual(lineText(text, lines[0]), "wrap", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 0 text");
  expectEqual(lineText(text, lines[1]), "these", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 1 text");
  expectEqual(lineText(text, lines[2]), "words", "keepsOffsetsIncreasingAcrossWrappedOutput", "line 2 text");

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
  measureCallCount = 0;
  const std::string text =
      "\xC3\xA9"
      "\xC3\xA9"
      "\xC3\xA9";
  const auto lines = wrapWithFixedWidth(text, 2);

  expectEqual(lines.size(), 2, "avoidsSplittingUtf8SequencesMidByte", "line count");
  expectEqual(lineText(text, lines[0]),
              "\xC3\xA9"
              "\xC3\xA9",
              "avoidsSplittingUtf8SequencesMidByte", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "avoidsSplittingUtf8SequencesMidByte", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "avoidsSplittingUtf8SequencesMidByte", "line 0 endOffset");
  expectEqual(lineText(text, lines[1]), "\xC3\xA9", "avoidsSplittingUtf8SequencesMidByte", "line 1 text");
  expectEqual(lines[1].startOffset, 4, "avoidsSplittingUtf8SequencesMidByte", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "avoidsSplittingUtf8SequencesMidByte", "line 1 endOffset");
}

void reservesLineCapacityBeforePushLoop() {
  measureCallCount = 0;
  const std::string text = "a a a a a a a a a a a a a a a a a";
  const auto lines = wrapWithFixedWidth(text, 1);

  expectEqual(lines.size(), 17, "reservesLineCapacityBeforePushLoop", "line count");
  expectGreaterOrEqual(lines.capacity(), 17, "reservesLineCapacityBeforePushLoop", "line capacity");
}

}  // namespace

int main() {
  wrapsUsingMeasuredWidthNotCharacterCount();
  triesWholeWordsBeforeHardWrapping();
  hardWrapsLongWordsWithoutEllipsis();
  emitsOversizedCodepointAsSingleLine();
  keepsUtf8CodepointsWholeWhenMeasured();
  preservesBlankLines();
  keepsOffsetsIncreasingAcrossWrappedOutput();
  avoidsSplittingUtf8SequencesMidByte();
  reservesLineCapacityBeforePushLoop();
  std::cout << "WriterWrappedLayoutTest passed\n";
  return 0;
}

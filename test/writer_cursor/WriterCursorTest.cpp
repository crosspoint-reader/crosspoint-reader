#include <cstdlib>
#include <iostream>
#include <string>

#include "src/activities/writer/WriterCursor.h"

namespace {

void expectEqual(size_t actual, size_t expected, const char* testName) {
  if (actual == expected) {
    return;
  }

  std::cerr << "FAILED: " << testName << "\nExpected: " << expected << "\nActual:   " << actual << "\n";
  std::exit(1);
}

void expectTrue(bool condition, const char* testName, const char* message) {
  if (condition) {
    return;
  }

  std::cerr << "FAILED: " << testName << "\n" << message << "\n";
  std::exit(1);
}

void clampsToUtf8CodepointBoundaries() {
  const std::string text =
      "a\xC3\xA9"
      "\xF0\x9D\x84\x9E"
      "b";

  expectEqual(WriterCursor::clamp(text, 0), 0, "clamp keeps start");
  expectEqual(WriterCursor::clamp(text, 2), 1, "clamp moves inside 2-byte codepoint left");
  expectEqual(WriterCursor::clamp(text, 5), 3, "clamp moves inside 4-byte codepoint left");
  expectEqual(WriterCursor::clamp(text, 99), text.size(), "clamp bounds to end");
}

void detectsUtf8ContinuationBytes() {
  expectTrue(!WriterCursor::isUtf8ContinuationByte('a'), "detectsUtf8ContinuationBytes", "ASCII is not continuation");
  expectTrue(!WriterCursor::isUtf8ContinuationByte(0xC3), "detectsUtf8ContinuationBytes",
             "UTF-8 lead byte is not continuation");
  expectTrue(WriterCursor::isUtf8ContinuationByte(0xA9), "detectsUtf8ContinuationBytes",
             "UTF-8 continuation byte is detected");
}

void handlesEmptyAndAsciiText() {
  const std::string empty;
  const std::string ascii = "abc";

  expectEqual(WriterCursor::clamp(empty, 3), 0, "clamp bounds empty text");
  expectEqual(WriterCursor::moveLeft(empty, 0), 0, "moveLeft keeps empty text at start");
  expectEqual(WriterCursor::moveRight(empty, 0), 0, "moveRight keeps empty text at start");

  expectEqual(WriterCursor::moveLeft(ascii, ascii.size()), 2, "moveLeft steps across ASCII");
  expectEqual(WriterCursor::moveRight(ascii, 1), 2, "moveRight steps across ASCII");
}

void keepsCombiningMarksAttachedToBaseCodepoint() {
  const std::string text =
      "e\xCC\x81"
      "x";

  expectEqual(WriterCursor::clamp(text, 2), 1, "clamp backs out of combining continuation byte");
  expectEqual(WriterCursor::moveRight(text, 0), 3, "moveRight skips base plus combining mark");
  expectEqual(WriterCursor::moveRight(text, 1), 3, "moveRight keeps combining mark with base");
  expectEqual(WriterCursor::moveRight(text, 2), 3, "moveRight clamps inside combining continuation byte");
  expectEqual(WriterCursor::moveLeft(text, text.size()), 3, "moveLeft steps left from ASCII tail");
  expectEqual(WriterCursor::moveLeft(text, 3), 0, "moveLeft skips combining mark plus base");
  expectEqual(WriterCursor::moveLeft(text, 2), 0, "moveLeft clamps inside combining continuation byte");
}

void movesLeftByWholeCodepoints() {
  const std::string text =
      "a\xC3\xA9"
      "\xF0\x9D\x84\x9E"
      "b";

  expectEqual(WriterCursor::moveLeft(text, text.size()), 7, "moveLeft from end");
  expectEqual(WriterCursor::moveLeft(text, 7), 3, "moveLeft over 1-byte codepoint");
  expectEqual(WriterCursor::moveLeft(text, 3), 1, "moveLeft over 4-byte codepoint");
  expectEqual(WriterCursor::moveLeft(text, 1), 0, "moveLeft to start");
}

void movesRightByWholeCodepoints() {
  const std::string text =
      "a\xC3\xA9"
      "\xF0\x9D\x84\x9E"
      "b";

  expectEqual(WriterCursor::moveRight(text, 0), 1, "moveRight from start");
  expectEqual(WriterCursor::moveRight(text, 1), 3, "moveRight over 2-byte codepoint");
  expectEqual(WriterCursor::moveRight(text, 3), 7, "moveRight over 4-byte codepoint");
  expectEqual(WriterCursor::moveRight(text, 7), text.size(), "moveRight to end");
}

}  // namespace

int main() {
  clampsToUtf8CodepointBoundaries();
  detectsUtf8ContinuationBytes();
  handlesEmptyAndAsciiText();
  keepsCombiningMarksAttachedToBaseCodepoint();
  movesLeftByWholeCodepoints();
  movesRightByWholeCodepoints();
  std::cout << "WriterCursorTest passed\n";
  return 0;
}

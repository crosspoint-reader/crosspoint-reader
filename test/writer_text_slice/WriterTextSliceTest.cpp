#include <cstdlib>
#include <iostream>
#include <string>

#include "src/activities/writer/WriterTextSlice.h"

namespace {

void fail(const char* testName, const std::string& message) {
  std::cerr << "FAILED: " << testName << "\n" << message << "\n";
  std::exit(1);
}

void expectEqual(const std::string& actual, const std::string& expected, const char* testName) {
  if (actual == expected) {
    return;
  }

  fail(testName, "expected [" + expected + "], got [" + actual + "]");
}

void slicesValidRange() { expectEqual(WriterTextSlice::slice("abcdef", 1, 4), "bcd", "slicesValidRange"); }

void clampsEndPastText() { expectEqual(WriterTextSlice::slice("abcdef", 3, 99), "def", "clampsEndPastText"); }

void returnsEmptyWhenStartIsPastText() {
  expectEqual(WriterTextSlice::slice("abcdef", 99, 100), "", "returnsEmptyWhenStartIsPastText");
}

void returnsEmptyWhenEndPrecedesStart() {
  expectEqual(WriterTextSlice::slice("abcdef", 4, 2), "", "returnsEmptyWhenEndPrecedesStart");
}

}  // namespace

int main() {
  slicesValidRange();
  clampsEndPastText();
  returnsEmptyWhenStartIsPastText();
  returnsEmptyWhenEndPrecedesStart();
  std::cout << "WriterTextSliceTest passed\n";
  return 0;
}

#include <iostream>
#include <string>

#include "src/activities/home/HomeSelectorMemory.h"

namespace {

bool expectEqual(const std::string& testName, int expected, int actual) {
  if (expected == actual) {
    std::cout << "[PASS] " << testName << std::endl;
    return true;
  }

  std::cerr << "[FAIL] " << testName << " (expected " << expected << ", got " << actual << ")" << std::endl;
  return false;
}

}  // namespace

int main() {
  int failed = 0;

  {
    HomeSelectorMemory memory;
    failed += !expectEqual("Default selection starts at first item", 0, memory.restore(5));
  }

  {
    HomeSelectorMemory memory;
    memory.store(3, 5);
    failed += !expectEqual("Restores previously stored index", 3, memory.restore(5));
    failed += !expectEqual("Exposes stored index for deferred restore", 3, memory.getStoredIndex());
  }

  {
    HomeSelectorMemory memory;
    memory.store(99, 5);
    failed += !expectEqual("Clamps stored index above menu bounds", 4, memory.restore(5));
  }

  {
    HomeSelectorMemory memory;
    memory.store(-10, 5);
    failed += !expectEqual("Clamps stored index below menu bounds", 0, memory.restore(5));
  }

  {
    HomeSelectorMemory memory;
    memory.store(4, 5);
    failed += !expectEqual("Clamps restored index when menu shrinks", 2, memory.restore(3));
  }

  {
    HomeSelectorMemory memory;
    memory.store(2, 5);
    failed += !expectEqual("Empty menus safely restore to index 0", 0, memory.restore(0));
  }

  {
    HomeSelectorMemory memory;
    memory.store(2, 0);
    failed += !expectEqual("Storing with empty menu resets to index 0", 0, memory.restore(4));
  }

  if (failed > 0) {
    std::cerr << failed << " HomeSelectorMemory test(s) failed." << std::endl;
    return 1;
  }

  std::cout << "All HomeSelectorMemory tests passed." << std::endl;
  return 0;
}

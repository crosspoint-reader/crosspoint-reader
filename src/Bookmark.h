#pragma once

#include <cstdint>
#include <string>

struct Bookmark {
  uint16_t spineIndex;
  uint16_t pageNumber;
  std::string name;  // optional user-provided label (empty = use default)
};

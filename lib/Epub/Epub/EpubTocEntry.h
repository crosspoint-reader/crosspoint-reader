#pragma once

#include <string>

struct EpubTocEntry {
  std::string title;
std::string href;
std::string anchor;
uint8_t level;
};

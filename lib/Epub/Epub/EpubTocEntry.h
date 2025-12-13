#pragma once

#include <string>

class EpubTocEntry {
 public:
  std::string title;
  std::string href;
  std::string anchor;
  int level;
  EpubTocEntry(std::string title, std::string href, std::string anchor, const int level)
      : title(std::move(title)), href(std::move(href)), anchor(std::move(anchor)), level(level) {}
};

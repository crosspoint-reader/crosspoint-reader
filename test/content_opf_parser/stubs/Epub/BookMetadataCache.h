#pragma once

#include <string>
#include <vector>

class BookMetadataCache {
 public:
  std::vector<std::string> spineHrefs;
  void createSpineEntry(const std::string& href) { spineHrefs.push_back(href); }
};

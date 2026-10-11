#pragma once

#include <Print.h>

#include <cstdint>
#include <string>
#include <vector>

class Epub;

struct EpubSearchResult {
  int spineIndex = 0;
  uint32_t visibleTextOffset = 0;
  std::string chapterTitle;
  std::string preContext;
  std::string match;
  std::string postContext;
};

class EpubSearch {
 public:
  static bool search(const Epub& epub, const std::string& query, std::vector<EpubSearchResult>& results,
                     size_t maxResults = 30);
};

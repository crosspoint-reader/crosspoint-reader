#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
  static bool searchHtml(std::string_view html, std::string_view query, int spineIndex, std::string_view chapterTitle,
                         std::vector<EpubSearchResult>& results, size_t maxResults = 30);
  static bool search(const Epub& epub, const std::string& query, std::vector<EpubSearchResult>& results,
                     size_t maxResults = 30);
};

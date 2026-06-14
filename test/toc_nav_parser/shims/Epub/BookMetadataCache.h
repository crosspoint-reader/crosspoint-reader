#pragma once

// Host stand-in for the real BookMetadataCache. The nav parser only calls
// createTocEntry(); this fake records the entries so tests can assert on what
// the parser chose to emit.
#include <cstdint>
#include <string>
#include <vector>

struct TocEntry {
  std::string title;
  std::string href;
  std::string anchor;
  uint8_t level;
};

class BookMetadataCache {
 public:
  std::vector<TocEntry> entries;

  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level) {
    entries.push_back({title, href, anchor, level});
  }
};

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Dictionary lookup against the three-file format produced by
// scripts/build_dict.py on /shortbread/dict/{pages.idx,words.idx,defs.bin}.
//
// Usage:
//   DictLookup dict;
//   if (dict.load()) {
//     std::string def = dict.lookup("cat");
//   }
//
// load() reads pages.idx (~10KB) into RAM. Call once when entering dict mode.
// lookup() reads at most one 4KB page from words.idx + one variable-length
// block from defs.bin per call. Returns empty string on miss.
class DictLookup {
 public:
  DictLookup() = default;
  ~DictLookup() = default;

  DictLookup(const DictLookup&) = delete;
  DictLookup& operator=(const DictLookup&) = delete;

  bool load();
  bool isLoaded() const { return loaded; }
  std::string lookup(const std::string& word) const;

 private:
  struct PageEntry {
    std::string firstWord;
  };

  bool loaded = false;
  uint32_t pageCount = 0;
  uint32_t pageSize = 0;
  uint32_t entryCount = 0;
  std::vector<PageEntry> pages;
};

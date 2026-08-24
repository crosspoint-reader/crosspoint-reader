#pragma once

#include "Print.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Epub {
 public:
  struct SpineItem {
    std::string href;
    std::size_t cumulativeSize = 0;
  };

  std::vector<SpineItem> spine;
  std::vector<std::string> contents;
  std::size_t fragmentSize = 1024;

  int getSpineItemsCount() const { return static_cast<int>(spine.size()); }
  std::size_t getBookSize() const { return spine.empty() ? 0 : spine.back().cumulativeSize; }
  SpineItem getSpineItem(int index) const { return spine.at(static_cast<std::size_t>(index)); }
  std::size_t getCumulativeSpineItemSize(int index) const {
    return spine.at(static_cast<std::size_t>(index)).cumulativeSize;
  }
  bool getItemSize(const std::string& href, std::size_t* size) const {
    for (std::size_t i = 0; i < spine.size(); i++) {
      if (spine[i].href == href) {
        if (size) *size = contents[i].size();
        return true;
      }
    }
    return false;
  }

  bool readItemContentsToStream(const std::string& href, Print& out, std::size_t, bool = false) const {
    for (std::size_t i = 0; i < spine.size(); i++) {
      if (spine[i].href != href) continue;
      const std::string& source = contents[i];
      const std::size_t chunk = fragmentSize == 0 ? 1 : fragmentSize;
      for (std::size_t pos = 0; pos < source.size(); pos += chunk) {
        const std::size_t n = std::min(chunk, source.size() - pos);
        out.write(reinterpret_cast<const std::uint8_t*>(source.data() + pos), n);
      }
      return true;
    }
    return false;
  }

  float calculateProgress(int, float) const { return 0.0f; }
};

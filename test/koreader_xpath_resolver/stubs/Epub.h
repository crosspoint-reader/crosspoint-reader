#pragma once

#include <Print.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Epub {
 public:
  struct SpineItem {
    std::string href;
  };

  explicit Epub(std::vector<std::string> spineContents) : spineContents(std::move(spineContents)) {}

  int getSpineItemsCount() const { return static_cast<int>(spineContents.size()); }

  SpineItem getSpineItem(const int spineIndex) const {
    if (spineIndex < 0 || spineIndex >= getSpineItemsCount()) {
      return {};
    }
    return {"chapter" + std::to_string(spineIndex) + ".xhtml"};
  }

  bool readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                bool = false) const {
    for (int spineIndex = 0; spineIndex < getSpineItemsCount(); spineIndex++) {
      if (itemHref != getSpineItem(spineIndex).href) {
        continue;
      }

      const auto& contents = spineContents[spineIndex];
      size_t offset = 0;
      while (offset < contents.size()) {
        const size_t size = std::min(chunkSize, contents.size() - offset);
        out.write(reinterpret_cast<const uint8_t*>(contents.data() + offset), size);
        offset += size;
      }
      return true;
    }
    return false;
  }

 private:
  std::vector<std::string> spineContents;
};

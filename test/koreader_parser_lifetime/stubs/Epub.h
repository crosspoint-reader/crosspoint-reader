#pragma once

#include <Print.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Epub {
 public:
  struct SpineEntry {
    std::string href;
  };
  explicit Epub(std::string chapter, size_t chunkSize = 0) : chapter(std::move(chapter)), chunkSize(chunkSize) {}
  int getSpineItemsCount() const { return 1; }
  SpineEntry getSpineItem(int index) const { return {index == 0 ? "chapter.xhtml" : ""}; }
  bool readItemContentsToStream(const std::string&, Print& output, size_t requestedSize) const {
    if (++reads == failRead) return false;
    const size_t step = chunkSize > 0 ? chunkSize : requestedSize;
    for (size_t offset = 0; offset < chapter.size(); offset += step) {
      const size_t count = std::min(step, chapter.size() - offset);
      if (output.write(reinterpret_cast<const uint8_t*>(chapter.data() + offset), count) != count) return false;
    }
    return true;
  }
  mutable unsigned reads = 0;
  unsigned failRead = 0;

 private:
  std::string chapter;
  size_t chunkSize;
};

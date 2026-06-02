#pragma once
#include <Print.h>

#include <string>

class Epub {
 public:
  struct SpineEntry {
    std::string href;
  };

  int getSpineItemsCount() const;
  SpineEntry getSpineItem(int spineIndex) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
};

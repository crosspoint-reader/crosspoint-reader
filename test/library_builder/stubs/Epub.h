#pragma once

#include <map>
#include <string>

#include "HalStorage.h"

class BookMetadataCache {
 public:
  struct ExtendedMetadata {
    std::string titleSort;
    std::string authorSort;
    std::string series;
    std::string seriesIndex;
    std::string tags;
  };
};

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  std::string titleSort;
  std::string authorSort;
  bool success = true;
};

inline std::map<std::string, FakeMetadata> bookMetadata;

class Epub {
  std::string path;

 public:
  Epub(const std::string& path, const char*) : path(path) {}

  bool loadMetadata(std::string& title, std::string& author, BookMetadataCache::ExtendedMetadata* extended = nullptr) {
    ++fake::parses;
    if (extended) *extended = {};
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    if (extended) {
      extended->titleSort = metadata.titleSort;
      extended->authorSort = metadata.authorSort;
    }
    return true;
  }
};

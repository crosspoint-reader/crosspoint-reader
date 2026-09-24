#pragma once

#include <map>
#include <string>

#include "HalStorage.h"

// The one piece of BookMetadataCache the builder sees: the package metadata
// loadMetadata() fills.
class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
  };
};

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  std::string language;
  bool success = true;
};

inline std::map<std::string, FakeMetadata> bookMetadata;

class Epub {
  std::string path;

 public:
  Epub(const std::string& path, const char*) : path(path) {}

  bool loadMetadata(BookMetadataCache::BookMetadata& out) {
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    out = BookMetadataCache::BookMetadata{};
    out.title = metadata.title;
    out.author = metadata.author;
    out.language = metadata.language;
    return true;
  }
};

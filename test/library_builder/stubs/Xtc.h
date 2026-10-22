#pragma once

#include <string>

#include "Epub.h"

// Shares bookMetadata with the Epub stub: an XTC/XTCH "file" reports what the test put there.
class Xtc {
  std::string path;
  std::string title;
  std::string author;

 public:
  Xtc(std::string path, const std::string&) : path(std::move(path)) {}

  bool load() {
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    return true;
  }
  std::string getTitle() const { return title; }
  std::string getAuthor() const { return author; }
};

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "HalStorage.h"

struct PackageGroupFields {
  std::string series;
  std::optional<float> seriesIndex;
  std::string publisher;
  std::string language;
  std::string subject;
};

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  std::string series;
  std::optional<float> seriesIndex;
  std::string publisher;
  std::string language;
  std::string subject;
  bool success = true;
};

inline std::map<std::string, FakeMetadata> bookMetadata;

class Epub {
  std::string path;

 public:
  Epub(const std::string& path, const char*) : path(path) {}

  bool loadMetadata(std::string& title, std::string& author, PackageGroupFields* fields = nullptr) {
    ++(fields ? fake::groupedLoads : fake::plainLoads);
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    if (fields) {
      fields->series = metadata.series;
      fields->seriesIndex = metadata.seriesIndex;
      fields->publisher = metadata.publisher;
      fields->language = metadata.language;
      fields->subject = metadata.subject;
    }
    return true;
  }
};

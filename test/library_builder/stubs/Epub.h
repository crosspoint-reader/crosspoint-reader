#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "HalStorage.h"

struct PackageGroupFields {
  std::string series;
  std::string seriesIndexText;
  std::string publisher;
  std::string language;
  std::string subject;
};

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  std::string series;
  std::string seriesIndexText;
  std::string publisher;
  std::string language;
  std::string subject;
  bool success = true;
  // Return Cached from title/author reads when true.
  bool cached = false;
};

inline std::map<std::string, FakeMetadata> bookMetadata;

enum class MetadataCachePolicy : uint8_t { Allow, Bypass };
enum class MetadataSource : uint8_t { Failed, Cached, Parsed };

class Epub {
  std::string path;

 public:
  Epub(const std::string& path, const char*) : path(path) {}

  MetadataSource loadMetadata(std::string& title, std::string& author, const MetadataCachePolicy policy) {
    ++fake::cachedLoads;
    ++fake::parses;
    if (policy == MetadataCachePolicy::Bypass) ++fake::bypassLoads;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return MetadataSource::Failed;
    title = metadata.title;
    author = metadata.author;
    return metadata.cached ? MetadataSource::Cached : MetadataSource::Parsed;
  }

  MetadataSource loadMetadata(std::string& title, std::string& author, PackageGroupFields& fields) {
    ++fake::groupedLoads;
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return MetadataSource::Failed;
    title = metadata.title;
    author = metadata.author;
    fields.series = metadata.series;
    fields.seriesIndexText = metadata.seriesIndexText;
    fields.publisher = metadata.publisher;
    fields.language = metadata.language;
    fields.subject = metadata.subject;
    return MetadataSource::Parsed;
  }
};

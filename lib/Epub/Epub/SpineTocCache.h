#pragma once

#include <SD.h>

#include <fstream>
#include <string>

class SpineTocCache {
 public:
  struct SpineEntry {
    std::string href;
    size_t cumulativeSize;
    int16_t tocIndex;

    SpineEntry() : cumulativeSize(0), tocIndex(-1) {}
    SpineEntry(std::string href, size_t cumulativeSize, int16_t tocIndex)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, uint8_t level, int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  uint16_t spineCount;
  uint16_t tocCount;
  bool loaded;
  bool buildMode;

  // Temp file handles during build
  File spineFile;
  File tocFile;

  void writeSpineEntry(File& file, const SpineEntry& entry) const;
  void writeTocEntry(File& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(std::ifstream& is) const;
  TocEntry readTocEntry(std::ifstream& is) const;

 public:
  explicit SpineTocCache(std::string cachePath)
      : cachePath(std::move(cachePath)), spineCount(0), tocCount(0), loaded(false), buildMode(false) {}
  ~SpineTocCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  void addSpineEntry(const std::string& href);
  void addTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endWrite();

  // Post-processing to update mappings and sizes
  bool updateMappingsAndSizes(const std::string& epubPath) const;

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index) const;
  TocEntry getTocEntry(int index) const;
  int getSpineCount() const;
  int getTocCount() const;
  bool isLoaded() const;
};

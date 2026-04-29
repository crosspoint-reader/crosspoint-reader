#pragma once

#include <cstdint>
#include <string>
#include <vector>

class AnnotationsManager {
 public:
  struct Rect {
    int16_t x, y, w, h;
    uint16_t sectionPage;  // absolute page index within section
  };

  struct AnnotationRecord {
    uint16_t sectionIdx;
    std::vector<Rect> rects;
  };

  // Load annotations from .crosspoint/epub_<hash>/annotations.bin
  bool load(const char* bookCachePath);
  // Save current annotations to disk
  bool save(const char* bookCachePath) const;

  void add(AnnotationRecord record);

  std::vector<AnnotationRecord> forSection(uint16_t sectionIdx) const;
  bool hasAnnotationsForSection(uint16_t sectionIdx) const;
  bool empty() const { return records.empty(); }
  size_t size() const { return records.size(); }

 private:
  static constexpr uint8_t FILE_VERSION = 3;

  std::vector<AnnotationRecord> records;

  static std::string annotationsPath(const char* bookCachePath);
};

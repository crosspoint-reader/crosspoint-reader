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
    std::vector<Rect> rects;       // pixel rects for drawing underlines
    std::string textPreview;       // first 100 chars for matching on permanent delete
  };

  // Load annotations from .crosspoint/epub_<hash>/annotations.bin
  bool load(const char* bookCachePath);
  // Save current annotations to disk
  bool save(const char* bookCachePath) const;

  void add(AnnotationRecord record);

  // Remove only the metadata (annotations.bin entry) — text file untouched
  void removeMeta(size_t idx);

  // Remove metadata AND delete matching entry from the clipping txt file (streaming, no full-file RAM read)
  bool permanentDelete(size_t idx, const char* clippingFilePath);

  std::vector<AnnotationRecord> forSection(uint16_t sectionIdx) const;
  bool hasAnnotationsForSection(uint16_t sectionIdx) const;
  // Returns global index of first annotation for section, or SIZE_MAX if none
  size_t firstIndexForSection(uint16_t sectionIdx) const;
  bool empty() const { return records.empty(); }
  size_t size() const { return records.size(); }

 private:
  static constexpr uint8_t FILE_VERSION = 2;
  static constexpr size_t MAX_PREVIEW_LEN = 100;
  static constexpr size_t STREAM_BLOCK = 512;

  std::vector<AnnotationRecord> records;

  static std::string annotationsPath(const char* bookCachePath);
};

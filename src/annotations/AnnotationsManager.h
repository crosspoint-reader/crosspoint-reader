#pragma once

#include <cstdint>
#include <string>
#include <vector>

class AnnotationsManager {
 public:
  struct AnnotationRecord {
    uint16_t sectionIdx;
    uint16_t sectionPage;
    uint16_t endSectionPage;
    uint16_t wordCount;
    std::string startText;
    std::string endText;
    std::string beforeStartText;
    std::string afterEndText;
  };

  bool load(const char* bookCachePath);
  bool save(const char* bookCachePath) const;

  void add(AnnotationRecord record);

  std::vector<AnnotationRecord> forSection(uint16_t sectionIdx) const;
  bool hasAnnotationsForSection(uint16_t sectionIdx) const;
  bool empty() const { return records.empty(); }
  size_t size() const { return records.size(); }

 private:
  static constexpr uint8_t FILE_VERSION = 6;

  std::vector<AnnotationRecord> records;

  static std::string annotationsPath(const char* bookCachePath);
};

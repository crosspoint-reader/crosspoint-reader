#pragma once

#include <cstdint>
#include <string>

class BookProgressDataStore {
 public:
  enum class BookKind : uint8_t {
    Unknown = 0,
    Epub,
    Txt,
    Markdown,
    Xtc,
  };

  struct ProgressData {
    BookKind kind = BookKind::Unknown;
    float percent = 0.0f;
    uint32_t page = 0;       // 1-based page number for display
    uint32_t pageCount = 0;  // Section page count for EPUB, whole-book page count for other formats
    int32_t spineIndex = -1;
  };

  static bool supportsBookPath(const std::string& bookPath);
  static bool resolveCachePath(const std::string& bookPath, std::string& outCachePath);
  static bool loadProgress(const std::string& bookPath, ProgressData& outProgress);
  static const char* kindName(BookKind kind);
  static std::string formatPositionLabel(const ProgressData& progress);
};

#pragma once
#include <cstdint>

struct ScreenshotInfo {
  enum class ReaderType : uint8_t { None, Epub, Txt, Xtc };
  ReaderType readerType = ReaderType::None;
  char title[64] = {};      // Sanitized, truncated book title (null-terminated)
  int spineIndex = -1;      // EPUB only: current spine/chapter index
  int currentPage = 0;      // 1-based page number
  int totalPages = 0;       // Total pages in chapter (EPUB) or book (TXT/XTC)
  int progressPercent = 0;  // 0-100 whole-book progress
};

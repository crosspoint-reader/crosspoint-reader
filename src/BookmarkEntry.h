#pragma once
#include <cstdint>
#include <string>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  uint8_t bookPercent;        // 0-100 overall book progress
  uint16_t chapterPageCount;  // Total page count of the chapter at the time of bookmarking
  uint16_t chapterProgress;   // Number of pages into the chapter at the time of bookmarking
  uint16_t spineIndex;        // Spine item index
  uint16_t pageIndex;         // Page index within spine item
  std::string summary;        // First few words of a page to help identify it
};
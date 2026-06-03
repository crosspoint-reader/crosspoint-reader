#pragma once
#include <cstdint>
#include <string>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  std::string xpath;    // XPath-like progress string
  std::string summary;  // First few words of a page to help identify it
  float percentage;     // Progress percentage (0.0 to 1.0)

  uint16_t computedSpineIndex = 0;        // Spine index at the time of bookmarking
  uint16_t computedChapterPageCount = 0;  // Total page count of the chapter at the time of bookmarking
  uint16_t computedChapterProgress = 0;   // Number of pages into the chapter at the time of bookmarking
};
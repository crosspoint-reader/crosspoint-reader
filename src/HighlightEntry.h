#pragma once
#include <cstdint>
#include <string>

// A single highlight entry — a span of selected text in a book.
//
// Mirrors the BookmarkEntry design: xpath + percentage make the position robust
// across re-layouts, while the page/element/word coordinates allow best-effort
// re-display in the reader when the rendering settings (and therefore the cached
// page geometry) still match the values captured at creation time.
struct HighlightEntry {
  std::string text;         // Selected text (words joined with single spaces)
  std::string xpath;        // XPath-like progress string at the start of the selection
  float percentage = 0.0f;  // Book progress (0.0 to 1.0) at the start, used for sorting + display

  uint16_t spineIndex = 0;  // Chapter (spine) index — primary sort key for export

  // Geometry for best-effort re-display. Page-local element/word indices into the
  // cached Page layout. Only used when chapterPageCount matches the current
  // section page count (i.e. the cached geometry is still valid).
  uint16_t startPage = 0;
  uint16_t startElement = 0;
  uint16_t startWord = 0;
  uint16_t endPage = 0;
  uint16_t endElement = 0;
  uint16_t endWord = 0;

  // Total page count of the chapter at creation time. When this differs from the
  // current section page count, the rendering settings changed and the stored
  // geometry no longer maps cleanly, so re-display is skipped (text stays valid).
  uint16_t chapterPageCount = 0;
};

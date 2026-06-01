#pragma once
#include <cstdint>
#include <string>

// A single highlight entry — a span of selected text in a book.
//
// Re-display in the reader is done by matching the stored text against the words
// of the current page (not by stored pixel/word coordinates), so highlights stay
// correct across font, margin, and orientation changes that re-flow the layout.
struct HighlightEntry {
  std::string text;         // Selected text (words joined with single spaces)
  std::string xpath;        // XPath-like progress string at the start of the selection
  float percentage = 0.0f;  // Book progress (0.0 to 1.0) at the start, used for sorting + display

  uint16_t spineIndex = 0;  // Chapter (spine) index — primary sort key for export
};

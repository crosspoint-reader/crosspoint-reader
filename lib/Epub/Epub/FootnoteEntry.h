#pragma once

#include <cstring>

// 320 bytes per entry. Sized generously because:
// - number[64]: link display text can be a full sentence fragment (e.g. "See Chapter 5")
// - href[256]: EPUB relative paths with fragment IDs can be deeply nested
// Footnote lists are small (typically <20 entries per page) so total cost is modest.
struct FootnoteEntry {
  char number[64] = {};   // Link display text (e.g. "[1]", "See Chapter 5", or full sentence fragment)
  char href[256] = {};    // Target href — long enough for deep relative EPUB paths and fragment identifiers

  FootnoteEntry() = default;
};

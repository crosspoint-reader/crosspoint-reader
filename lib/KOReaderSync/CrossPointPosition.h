#pragma once

#include <cstdint>

/**
 * CrossPoint position representation.
 */
struct CrossPointPosition {
  int spineIndex;                  // Current spine item (chapter) index
  int pageNumber;                  // Current page within the spine item
  int totalPages;                  // Total pages in the current spine item
  uint32_t visibleTextOffset = 0;  // Authoritative zero-based visible codepoint offset
  bool hasVisibleTextOffset = false;
  uint16_t paragraphIndex = 0;         // 1-based synthetic paragraph index from XPath p[N]
  bool hasParagraphIndex = false;      // True when paragraphIndex was resolved from XPath
  uint16_t liIndex = 0;                // Running <li> count at the matched XPath element
  bool hasLiIndex = false;             // True when target element is <li> and liIndex was resolved
  char xpathAnchorId[64] = {};         // First <a id> captured inside the matched XPath element
  bool hasResolvedSpineIndex = false;  // Spine came from local state or a content anchor, not percentage
  bool hasMappedPage = false;          // Page came from local layout data, not percentage scaling
};

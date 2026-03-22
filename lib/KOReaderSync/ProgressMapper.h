#pragma once
#include <Epub.h>

#include <memory>
#include <string>

/**
 * CrossPoint position representation.
 */
struct CrossPointPosition {
  int spineIndex;                  // Current spine item (chapter) index
  int pageNumber;                  // Current page within the spine item (estimated if no paragraph LUT)
  int totalPages;                  // Total pages in the current spine item
  uint16_t paragraphIndex = 0;     // 1-based <p> index from XPath (0 if unavailable)
  bool hasParagraphIndex = false;  // True when paragraphIndex was resolved from XPath
};

/**
 * KOReader position representation.
 */
struct KOReaderPosition {
  std::string xpath;  // XPath-like progress string
  float percentage;   // Progress percentage (0.0 to 1.0)
};

/**
 * Maps between CrossPoint and KOReader position formats.
 *
 * CrossPoint tracks position as (spineIndex, pageNumber).
 * KOReader uses XPath-like strings + percentage.
 *
 * Forward mapping (CrossPoint -> KOReader):
 * - Prefer element-level XPath extracted from current spine XHTML.
 * - Fallback to synthetic chapter XPath if extraction fails.
 *
 * Reverse mapping (KOReader -> CrossPoint):
 * - Prefer incoming XPath (DocFragment + element path) when resolvable.
 * - Fallback to percentage-based approximation when XPath is missing/invalid.
 *
 * This keeps behavior stable on low-memory devices while improving round-trip
 * sync precision when KOReader provides detailed paths.
 */
class ProgressMapper {
 public:
  /**
   * Convert CrossPoint position to KOReader format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return KOReader position
   */
  static KOReaderPosition toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  /**
   * Convert KOReader position to CrossPoint format.
   *
   * Uses XPath-first resolution when possible and percentage fallback otherwise.
   * Returned pageNumber can still be approximate because page counts differ
   * across renderer/font/layout settings.
   *
   * @param epub The EPUB book
   * @param koPos KOReader position
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                         int currentSpineIndex = -1, int totalPagesInCurrentSpine = 0);

 private:
  /**
   * Generate XPath for KOReader compatibility.
   * Fallback format: /body/DocFragment[spineIndex + 1]/body
   */
  static std::string generateXPath(int spineIndex);
};

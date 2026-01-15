#pragma once
#include <Epub.h>

#include <memory>
#include <string>

/**
 * CrossPoint position representation.
 */
struct CrossPointPosition {
  int spineIndex;  // Current spine item (chapter) index
  int pageNumber;  // Current page within the spine item
  int totalPages;  // Total pages in the current spine item
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
 * Since CrossPoint discards HTML structure during parsing, we generate
 * synthetic XPath strings based on spine index, using percentage as the
 * primary sync mechanism.
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
   * Note: The returned pageNumber may be approximate since different
   * rendering settings produce different page counts.
   *
   * @param epub The EPUB book
   * @param koPos KOReader position
   * @param totalPagesInSpine Total pages in the target spine item (for page estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                         int totalPagesInSpine = 0);

 private:
  /**
   * Generate XPath for KOReader compatibility.
   * Format: /body/DocFragment[spineIndex+1]/body/p[estimatedParagraph]
   * Paragraph is estimated based on page position within the chapter.
   */
  static std::string generateXPath(int spineIndex, int pageNumber, int totalPages);

  /**
   * Parse DocFragment index from XPath string.
   * Returns -1 if not found.
   */
  static int parseDocFragmentIndex(const std::string& xpath);
};

#pragma once

#include <Epub.h>

#include <memory>
#include <string>

/**
 * Lightweight XPath/progress bridge for KOReader sync.
 *
 * Why this exists:
 * - CrossPoint stores reading position as chapter/page.
 * - KOReader sync uses XPath + percentage.
 *
 * This utility reparses exactly one spine XHTML item with Expat to translate
 * between the two formats.  It streams through the parse using O(1) memory
 * (no anchor list), so it handles arbitrarily large chapters without OOM.
 *
 * Design constraints (ESP32-C3):
 * - No persistent full-book structures.
 * - Parse-on-demand and free memory immediately.
 * - Keep fallback behavior deterministic if parsing/matching fails.
 */
class ChapterXPathIndexer {
 public:
  /**
   * Convert an intra-spine progress ratio to the nearest element-level XPath.
   *
   * @param epub Loaded EPUB instance
   * @param spineIndex Current spine item index
   * @param intraSpineProgress Position within the spine item [0.0, 1.0]
   * @return Best matching XPath for KOReader, or empty string on failure
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

  /**
   * Resolve a KOReader XPath to an intra-spine progress ratio.
   *
   * Matching strategy:
   * 1) exact anchor path match,
   * 2) index-insensitive path match,
   * 3) ancestor fallback.
   *
   * @param epub Loaded EPUB instance
   * @param spineIndex Spine item index to parse
   * @param xpath Incoming KOReader XPath
   * @param outIntraSpineProgress Resolved position within spine [0.0, 1.0]
   * @param outExactMatch True only for full exact path match
   * @return true if any match was resolved; false means caller should fallback
   */
  static bool findProgressForXPath(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpath,
                                   float& outIntraSpineProgress, bool& outExactMatch);

  /**
   * Parse DocFragment index from KOReader-style path segment:
   * /body/DocFragment[N]/body/...
   *
   * KOReader uses 1-based DocFragment indices; N is converted to the 0-based
   * spine index stored in outSpineIndex (i.e. outSpineIndex = N - 1).
   *
   * @param xpath KOReader XPath
   * @param outSpineIndex 0-based spine index derived from DocFragment[N]
   * @return true when DocFragment[N] exists and N is a valid integer >= 1
   *         (converted to 0-based outSpineIndex); false otherwise
   */
  static bool tryExtractSpineIndexFromXPath(const std::string& xpath, int& outSpineIndex);

  /**
   * Extract the paragraph index from a KOReader XPath.
   * Looks for the first /p[N] segment after /body/ and returns N (1-based).
   *
   * Example: "/body/DocFragment[7]/body/p[685]/text().96" → outParagraphIndex = 685
   *
   * @param xpath KOReader XPath
   * @param outParagraphIndex 1-based paragraph index
   * @return true if a /p[N] segment was found
   */
  static bool tryExtractParagraphIndexFromXPath(const std::string& xpath, uint16_t& outParagraphIndex);
};

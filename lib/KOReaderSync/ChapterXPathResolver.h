#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

class ChapterXPathResolver {
 public:
  /**
   * Resolve the Nth paragraph in a spine item to its real XHTML ancestry path.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]
   *
   * An empty string means parsing failed or the paragraph index was not found.
   */
  static std::string findXPathForParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex);

  /**
   * Resolve a zero-based visible-codepoint offset in a spine item to its real
   * XHTML ancestry path plus text-node offset.
   *
   * Offsets count every body codepoint the way ChapterHtmlSlimParser does, so the
   * text may live in any element (<p>, <div>, <span>, <li>, headings, ...).
   * Whitespace-only runs are never used as anchors; a target inside one resolves
   * at the start of the next text run.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text()[1].0
   *
   * An empty string means parsing failed or no text exists at or after the offset.
   */
  static std::string findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, int spineIndex,
                                                   uint32_t visibleTextOffset);

  /**
   * Resolve intra-spine progress to a real XHTML ancestry path plus text offset.
   * Progress is measured over all visible body text (see findXPathForVisibleTextOffset).
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text().96
   *
   * An empty string means parsing failed or the location could not be resolved.
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);
};

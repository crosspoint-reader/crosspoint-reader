#pragma once
#include <string>

// Path and text helpers for the highlight feature. Mirrors BookmarkUtil so the
// on-disk layout stays consistent: one flat JSON file per book under a dedicated
// directory on the SD card.
class HighlightUtil {
 public:
  // Directory holding one highlights JSON file per book.
  static std::string getHighlightsDir();

  // Map a book file path to its flat highlights JSON path (slashes flattened to
  // underscores, extension replaced with .json), e.g.
  //   "/books/foo/bar.epub" -> "/.crosspoint/highlights/books_foo_bar.json"
  static std::string getHighlightPath(const std::string& bookPath);

  // Normalize selected text for storage: collapse runs of whitespace, strip
  // newlines, trim ends. Unlike bookmark summaries this is NOT length-capped —
  // a highlight is the full selected span.
  static std::string sanitizeHighlightText(std::string text);
};

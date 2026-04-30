#pragma once
#include <string>

/**
 * Converts HTML to plain text by stripping tags and decoding entities.
 * Designed for constrained RAM: output is always smaller than input.
 */
class HtmlToText {
 public:
  /**
   * Strip HTML tags and decode entities, returning readable plain text.
   * Block-level elements (p, div, h1-h6, li, etc.) become newlines.
   * script/style blocks are removed entirely.
   * Consecutive blank lines are collapsed to a single blank line.
   *
   * @param html       Raw HTML source
   * @param maxBytes   If non-zero, only the first maxBytes of html are processed.
   */
  static std::string convert(const std::string& html, size_t maxBytes = 0);

  /**
   * Extract the <title> text from an HTML document.
   * Returns an empty string if no title element is found.
   */
  static std::string extractTitle(const std::string& html);
};

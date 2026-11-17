#pragma once

#include <string>

// Find the closing '>' of a tag without mistaking quoted attribute text for markup.
// Returns npos for an incomplete tag or non-tag '<'; limit is exclusive.
bool htmlTagStart(const std::string& input, size_t pos);
size_t htmlTagEnd(const std::string& input, size_t pos, size_t limit);

// Convert an HTML fragment to plain text for the dictionary's plain-text
// fallback. Styling is ignored: block elements become line breaks and HTML
// entities are decoded. A heading becomes a label line of its own: the
// headingMarker byte is written at the start of that line, and the definition
// view draws a line starting with it in bold.
// Reuses the input buffer; move an owned string to avoid copying it.
std::string htmlToPlainText(std::string html, char headingMarker);

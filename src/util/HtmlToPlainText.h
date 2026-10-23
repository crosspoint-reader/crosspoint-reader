#pragma once

#include <string>

// Convert an HTML fragment to plain text for the dictionary's plain-text
// fallback. Styling is ignored: block elements become line breaks and HTML
// entities are decoded. A heading becomes a label line of its own: the
// headingMarker byte is written at the start of that line, and the definition
// view draws a line starting with it in bold.
std::string htmlToPlainText(const std::string& html, char headingMarker);

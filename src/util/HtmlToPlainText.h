#pragma once

#include <string>

// Convert an HTML fragment to readable plain text. This intentionally ignores
// styling; block elements become line breaks and HTML entities are decoded.
std::string htmlToPlainText(const std::string& html);

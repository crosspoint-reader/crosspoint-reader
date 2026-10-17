#pragma once

#include <string>

// Convert an HTML fragment to readable plain text. This intentionally ignores
// styling; block elements become line breaks and HTML entities are decoded.
// Reuses the input buffer; move an owned string to avoid copying it.
std::string htmlToPlainText(std::string html);

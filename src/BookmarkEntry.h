#pragma once
#include <cstdint>
#include <string>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  std::string xpath;                // XPath-like progress string
  float percentage;                 // Progress percentage (0.0 to 1.0)
  std::string summary;              // First few words of a page to help identify it
};
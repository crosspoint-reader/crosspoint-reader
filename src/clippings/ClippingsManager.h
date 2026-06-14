#pragma once

#include <string>

class ClippingsManager {
 public:
  // Appends a clipping entry to the configured clipping file.
  // File path is resolved dynamically from SETTINGS (single file or per-book).
  static bool saveClipping(const std::string& bookTitle, const std::string& author, const std::string& chapterTitle,
                           int pageNumber, const std::string& selectedText);

  // Returns the clipping file path for the given book title based on current SETTINGS.
  static std::string resolveClippingPath(const std::string& bookTitle);

  static constexpr const char* CLIPPINGS_PATH = "/My Clippings.txt";
  static constexpr const char* CLIPPINGS_DIR = "/clippings";
};

#pragma once

#include <string>

class ClippingsManager {
 public:
  // Appends a clipping entry to /My Clippings.txt on the SD card (Kindle-compatible filename).
  // Returns false if the SD write fails.
  static bool saveClipping(const std::string& bookTitle, const std::string& author, const std::string& chapterTitle,
                           int pageNumber, const std::string& selectedText);

  static constexpr const char* CLIPPINGS_PATH = "/My Clippings.txt";
};

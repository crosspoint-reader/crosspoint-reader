#pragma once

#include <string>

class ClippingsManager {
 public:
  // Appends a clipping entry to /clippings.txt on the SD card.
  // Returns false if the SD write fails.
  static bool saveClipping(const std::string& bookTitle, const std::string& author, const std::string& chapterTitle,
                           int pageNumber, const std::string& selectedText);

  static constexpr const char* CLIPPINGS_PATH = "/clippings.txt";
};

#pragma once
#include <SdFat.h>

#include <string>

// Exports the current page's text to a per-book .txt file on the SD card.
// Files are stored at /.crosspoint/exports/<sanitized-title>.txt
// Each capture is appended with chapter/page/percentage metadata.
class PageExporter {
 public:
  // Export a page's text. Returns true on success.
  // - bookTitle: e.g. "The Great Gatsby"
  // - bookAuthor: e.g. "F. Scott Fitzgerald"
  // - bookHash: fallback filename if title is empty
  // - chapterTitle: e.g. "Chapter 3: The River"
  // - pageNumber: 1-based page within chapter
  // - bookPercent: 0-100 overall book progress
  // - pageText: the plain text content of the page
  static bool exportPage(const std::string& bookTitle, const std::string& bookAuthor, const std::string& bookHash,
                         const std::string& chapterTitle, int pageNumber, int bookPercent, const std::string& pageText);

 private:
  static std::string sanitizeFilename(const std::string& title);
  static std::string getExportPath(const std::string& bookTitle, const std::string& bookHash);
  static bool writeHeader(FsFile& file, const std::string& bookTitle, const std::string& bookAuthor);
  static bool writeEntry(FsFile& file, const std::string& chapterTitle, int pageNumber, int bookPercent,
                         const std::string& pageText);
};

#pragma once
#include <SdFat.h>

#include <string>
#include <vector>

// A single captured page with its metadata.
struct CapturedPage {
  std::string pageText;
  std::string chapterTitle;
  int bookPercent;
  int chapterPercent;
};

// Exports captured passages to per-book .md files on the SD card.
// Files are stored at /Saved Passages/<book-filename>.md
// Each capture is appended with chapter/percentage metadata.
class PageExporter {
 public:
  // Export a passage (one or more captured pages). Returns true on success.
  // bookPath: full path to the book file (e.g. "/Books/My Book.epub")
  static bool exportPassage(const std::string& bookPath, const std::string& bookTitle, const std::string& bookAuthor,
                            const std::vector<CapturedPage>& pages);

 private:
  static std::string getExportPath(const std::string& bookPath);
  static bool writeHeader(FsFile& file, const std::string& bookTitle, const std::string& bookAuthor);
  static bool writePassage(FsFile& file, const std::vector<CapturedPage>& pages);
};

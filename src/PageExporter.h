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
// Files are stored at /Saved Passages/<sanitized-title>.md
// Each capture is appended with chapter/percentage metadata.
class PageExporter {
 public:
  // Export a passage (one or more captured pages). Returns true on success.
  static bool exportPassage(const std::string& bookTitle, const std::string& bookAuthor, const std::string& bookHash,
                            const std::vector<CapturedPage>& pages);

 private:
  static std::string sanitizeFilename(const std::string& title);
  static std::string getExportPath(const std::string& bookTitle, const std::string& bookHash);
  static bool writeHeader(FsFile& file, const std::string& bookTitle, const std::string& bookAuthor);
  static bool writePassage(FsFile& file, const std::vector<CapturedPage>& pages);
};

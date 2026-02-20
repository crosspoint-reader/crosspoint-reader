#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A single captured page with its metadata (used during capture, before saving).
struct CapturedPage {
  std::string pageText;
  std::string chapterTitle;
  int bookPercent;
  int chapterPercent;
  uint16_t spineIndex;
  uint16_t pageIndex;
};

// Metadata for a single clipping (stored in .idx file).
struct ClippingEntry {
  uint32_t textOffset;     // byte offset into .md where this clipping's text starts
  uint32_t textLength;     // byte length of the clipping text in .md
  uint8_t bookPercent;     // 0-100 overall book progress at capture start
  uint8_t chapterPercent;  // 0-100 chapter progress at capture start
  uint16_t spineIndex;     // spine item where capture started
  uint16_t startPage;      // first page captured (within spine item)
  uint16_t endPage;        // last page captured (within spine item)
};

// Stores clippings as two files per book:
//   /.crosspoint/clippings/<path-hash>.idx  (binary index)
//   /.crosspoint/clippings/<path-hash>.md   (formatted markdown for export)
//
// Index format: [magic:4 "CIDX"][version:1][count:2 LE][entries: count * 16 bytes]
// Each entry: [textOffset:4 LE][textLength:4 LE][bookPercent:1][chapterPercent:1]
//             [spineIndex:2 LE][startPage:2 LE][endPage:2 LE]
class ClippingStore {
 public:
  // Save a new clipping (appends to both .idx and .md). Returns true on success.
  static bool saveClipping(const std::string& bookPath, const std::string& bookTitle, const std::string& bookAuthor,
                           const std::vector<CapturedPage>& pages);

  // Load clipping index entries for a book.
  static std::vector<ClippingEntry> loadIndex(const std::string& bookPath);

  // Load the full text of a specific clipping from the .md file.
  static std::string loadClippingText(const std::string& bookPath, const ClippingEntry& entry);

  // Load a short preview of a clipping (first N characters).
  static std::string loadClippingPreview(const std::string& bookPath, const ClippingEntry& entry, int maxChars = 60);

  // Delete a clipping at the given index. Rewrites both files.
  static bool deleteClipping(const std::string& bookPath, int index);

  // Get the index file path for a book.
  static std::string getIndexPath(const std::string& bookPath);

  // Get the markdown file path for a book.
  static std::string getMdPath(const std::string& bookPath);

 private:
  static std::string getBasePath(const std::string& bookPath);
  static bool writeIndex(const std::string& path, const std::vector<ClippingEntry>& entries);
  static constexpr const char* CLIPPINGS_DIR = "/.crosspoint/clippings";
  static constexpr uint8_t FORMAT_VERSION = 1;
  static constexpr const char* INDEX_MAGIC = "CIDX";
  static constexpr const char* TAG = "CLP";
};

#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string series;
  std::string coverBmpPath;
  // -1 = use global setting, otherwise explicit per-book override.
  int8_t embeddedStyleOverride = -1;
  // -1 = use global setting, otherwise CrossPointSettings::IMAGE_RENDERING value.
  int8_t imageRenderingOverride = -1;
  // -1 = use global setting, otherwise CrossPointSettings::FONT_FAMILY value.
  int8_t fontFamilyOverride = -1;
  // Empty = use global setting, otherwise explicit SD-card family name override.
  std::string sdFontFamilyOverride;
  // -1 = use global setting, otherwise CrossPointSettings::FONT_SIZE value.
  int8_t fontSizeOverride = -1;
  // -1 = use global default, otherwise explicit per-book override (0 = off, 1 = on).
  int8_t bionicReadingOverride = -1;
  // -1 = use global setting, otherwise CrossPointSettings::PARAGRAPH_ALIGNMENT value.
  int8_t paragraphAlignmentOverride = -1;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore;
namespace JsonSettingsIO {
bool loadRecentBooks(RecentBooksStore& store, const char* json);
}  // namespace JsonSettingsIO

class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<RecentBook> recentBooks;

  friend bool JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);

 public:
  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author, const std::string& series,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& series, const std::string& coverBmpPath);

  // Remove a book from the recent list by path
  void removeBook(const std::string& path);

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  // Returns true if the book's file is missing from storage
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  bool saveToFile() const;

  bool loadFromFile();
  RecentBook getDataFromBook(std::string path) const;
  RecentBook getBookByPath(const std::string& path) const;
  bool setReaderOverrides(const std::string& path, int8_t embeddedStyleOverride, int8_t imageRenderingOverride);
  bool setReaderOverrides(const std::string& path, int8_t embeddedStyleOverride, int8_t imageRenderingOverride,
                          int8_t fontFamilyOverride, int8_t fontSizeOverride);
  bool setReaderOverrides(const std::string& path, int8_t embeddedStyleOverride, int8_t imageRenderingOverride,
                          int8_t fontFamilyOverride, const std::string& sdFontFamilyOverride, int8_t fontSizeOverride);
  bool setReaderOverrides(const std::string& path, int8_t embeddedStyleOverride, int8_t imageRenderingOverride,
                          bool bionicReadingOverride);
  bool setReaderOverrides(const std::string& path, int8_t embeddedStyleOverride, int8_t imageRenderingOverride,
                          int8_t fontFamilyOverride, int8_t fontSizeOverride, bool bionicReadingOverride);
  bool setReaderOverrides(const std::string& path, int8_t embeddedStyleOverride, int8_t imageRenderingOverride,
                          int8_t fontFamilyOverride, const std::string& sdFontFamilyOverride, int8_t fontSizeOverride,
                          bool bionicReadingOverride);
  bool setReaderOverrides(const std::string& path, int8_t embeddedStyleOverride, int8_t imageRenderingOverride,
                          int8_t fontFamilyOverride, const std::string& sdFontFamilyOverride, int8_t fontSizeOverride,
                          bool bionicReadingOverride, int8_t paragraphAlignmentOverride);

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()

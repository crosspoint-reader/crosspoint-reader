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
  // SidecarFiles::metadataStamp() of the metadata sidecar title/author/series were
  // taken with (0 = none). EPUB only; see refreshSidecarMetadata().
  uint32_t metadataStamp = 0;
  // -1 = use global setting, otherwise explicit per-book override.
  int8_t embeddedStyleOverride = -1;
  // -1 = use global setting, otherwise CrossPointSettings::IMAGE_RENDERING value.
  int8_t imageRenderingOverride = -1;
  // -1 = use global setting, otherwise CrossPointSettings::FONT_FAMILY value.
  int8_t fontFamilyOverride = -1;
  // Empty = use global setting, otherwise explicit SD-card family name override.
  std::string sdFontFamilyOverride;
  // -1 = use global setting, otherwise a point size (see CrossPointSettings::fontPointSize).
  int8_t fontSizeOverride = -1;
  // -1 = use global default, otherwise explicit per-book override (0 = off, 1 = on).
  int8_t bionicReadingOverride = -1;
  // -1 = use global setting, otherwise CrossPointSettings::PARAGRAPH_ALIGNMENT value.
  int8_t paragraphAlignmentOverride = -1;
  // -1 = use global default, otherwise explicit per-book override (0 = off, 1 = on).
  int8_t textAntiAliasingOverride = -1;
  // -1 = use global default, otherwise explicit per-book override (0 = off, 1 = on).
  int8_t hyphenationOverride = -1;
  // -1 = use global default, otherwise explicit per-book override (0 = off, 1 = on).
  int8_t fontSizeNormalizationOverride = -1;
  // -1 = use global default, otherwise explicit per-book override (0 = off, 1 = on).
  int8_t guideDotsOverride = -1;
  // -1 = use global default, otherwise explicit per-book override (0 = off, 1 = on).
  int8_t inlineFootnotePreviewsOverride = -1;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore;
namespace JsonSettingsIO {
bool loadRecentBooks(RecentBooksStore& store, const char* json);
}  // namespace JsonSettingsIO

// The list of books lately opened, newest first, with each book's per-book reader overrides.
//
// Kept in RAM only while something holds it (Hold), not for the whole session: loaded at boot it
// cost ~4.5 KB for good and, worse, split the heap's largest free block (61 KB -> 45 KB, X3), which
// is what the reader then opened every book with. The screens that show the list hold it while
// they are open; one-off calls (the reader adding its book, a per-book override saved, a book
// removed) hold it for the length of the call. Holds nest, so a call inside a held stretch reads
// the list already there.
class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  mutable std::vector<RecentBook> recentBooks;
  mutable bool loaded = false;
  uint8_t holds = 0;

  friend bool JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);

  void ensureLoaded() const;

 public:
  // Keeps the list in RAM for as long as it lives: the first Hold loads it from the card, the last
  // one to go lets it go. Hold one across anything that keeps a reference from getBooks().
  class Hold {
   public:
    Hold();
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
  };

  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author, const std::string& series,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& series, const std::string& coverBmpPath);

  // Re-read title, author and series for EPUB entries whose metadata sidecar was
  // added, edited or removed since they were stored. They are otherwise only
  // taken when the book is opened, so a sidecar written by the metadata-editor
  // plugin (or copied over USB) never reached the home screen. Covers the first
  // maxBooks entries still on the card, counted as the home screen counts them.
  // Call before copying entries out. Saves and returns true if any changed.
  bool refreshSidecarMetadata(size_t maxBooks);

  // Remove a book from the recent list by path
  void removeBook(const std::string& path);
  // A book renamed or moved on the card (BookRelocation): its row follows it, keeping its place and
  // its per-book overrides. Its cover thumbnail lives in the book's cache folder, which moved too.
  void relocate(const std::string& oldPath, const std::string& newPath, const std::string& oldCacheDir,
                const std::string& newCacheDir);

  // Get the list of recent books (most recent first). The reference is good while a Hold lives.
  // Called without one, the list is loaded and stays until the next Hold to end lets it go.
  const std::vector<RecentBook>& getBooks() const {
    ensureLoaded();
    return recentBooks;
  }

  // Get the count of recent books
  int getCount() const {
    ensureLoaded();
    return static_cast<int>(recentBooks.size());
  }

  // Returns true if the book's file is missing from storage
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  // Refuses while the list is not loaded: saving then would write an empty list over the file.
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
  // Master overload — covers every per-book override. The narrower overloads above
  // all funnel through here, preserving any fields they don't take as arguments.
  bool setReaderOverrides(const std::string& path, int8_t embeddedStyleOverride, int8_t imageRenderingOverride,
                          int8_t fontFamilyOverride, const std::string& sdFontFamilyOverride, int8_t fontSizeOverride,
                          int8_t bionicReadingOverride, int8_t paragraphAlignmentOverride,
                          int8_t textAntiAliasingOverride, int8_t hyphenationOverride,
                          int8_t fontSizeNormalizationOverride, int8_t guideDotsOverride,
                          int8_t inlineFootnotePreviewsOverride);
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()

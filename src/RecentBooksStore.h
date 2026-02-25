#pragma once
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
struct ThemeMetrics;

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  int16_t progressPercent = -1;
  uint32_t readingSeconds = 0;
  std::string progressValue = "--%";
  std::string metricsSubtitle = "0h 00m / --";

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

struct RecentBookListRowData {
  std::string authorSubtitle;
  std::string metricsRightText;
  int metricsRightX = 0;
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
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);
  void updateBookProgress(const std::string& path, int16_t progressPercent);
  void addBookReadingTime(const std::string& path, uint32_t elapsedSeconds);
  std::vector<RecentBookListRowData> buildListRowData(const std::vector<RecentBook>& books, const GfxRenderer& renderer,
                                                      const ThemeMetrics& metrics, int pageWidth,
                                                      int contentHeight) const;
  void drawMetricsOverlay(const GfxRenderer& renderer, const std::vector<RecentBookListRowData>& rowData,
                          size_t selectorIndex, int contentTop, int contentHeight, const ThemeMetrics& metrics) const;
  int getBookProgressPercent(const RecentBook& book) const;
  std::string getBookProgressValue(const RecentBook& book) const;
  std::string getBookReadingTime(const RecentBook& book) const;
  std::string getBookMetricsSubtitle(const RecentBook& book) const;

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  bool saveToFile() const;

  bool loadFromFile();
  RecentBook getDataFromBook(std::string path) const;

 private:
  void refreshComputedFields(RecentBook& book);
  void refreshAllComputedFields();
  static std::string formatDuration(uint32_t totalSeconds);
  static int clampProgressPercent(int progressPercent);
  static bool hasRemainingEstimate(const RecentBook& book);
  static uint32_t getRemainingSeconds(const RecentBook& book);
  bool loadFromBinaryFile();
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()

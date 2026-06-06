#pragma once
#include <ReadingStats.h>

#include <string>
#include <vector>

class ReadingStatsStore;
namespace JsonSettingsIO {
bool saveReadingStats(const ReadingStatsStore& store, const char* path);
bool loadReadingStats(ReadingStatsStore& store, const char* json);
}  // namespace JsonSettingsIO

// Singleton wrapper around the pure ReadingStatsAggregator that adds SD-card
// persistence. Mirrors RecentBooksStore. Reading data is stored at
// /.crosspoint/reading_stats.json.
class ReadingStatsStore {
  static ReadingStatsStore instance;
  reading_stats::ReadingStatsAggregator aggregator;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  // --- Session lifecycle (called from the reader) ---
  void beginSession(const std::string& bookPath, uint32_t nowMs) { aggregator.beginSession(bookPath, nowMs); }
  void recordPageTurn(uint32_t nowMs, bool forward) { aggregator.recordPageTurn(nowMs, forward); }
  // Ends the active session and persists. Best-effort: a failed save is logged.
  void endSession(uint32_t nowMs);

  // --- Persistence ---
  bool saveToFile() const;
  bool loadFromFile();

  // --- Accessors used by JsonSettingsIO and (later) the stats screen ---
  const std::vector<reading_stats::BookStats>& books() const { return aggregator.books(); }
  const reading_stats::BookStats* statsFor(const std::string& path) const { return aggregator.statsFor(path); }
  uint32_t totalPagesRead() const { return aggregator.totalPagesRead(); }
  uint32_t totalReadingMs() const { return aggregator.totalReadingMs(); }
  uint32_t pagesPerHour(const std::string& path) const { return aggregator.pagesPerHour(path); }

 private:
  ReadingStatsStore() = default;

  // Only JsonSettingsIO may replace the in-memory stats wholesale (used on load).
  void loadBooks(std::vector<reading_stats::BookStats> books) { aggregator.load(std::move(books)); }

  friend bool JsonSettingsIO::saveReadingStats(const ReadingStatsStore& store, const char* path);
  friend bool JsonSettingsIO::loadReadingStats(ReadingStatsStore& store, const char* json);
};

// Helper macro mirroring RECENT_BOOKS.
#define READING_STATS ReadingStatsStore::getInstance()

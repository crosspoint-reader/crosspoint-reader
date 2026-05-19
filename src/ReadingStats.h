#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// Per-book reading statistics. Keyed by KOReader document hash (or filename
// hash fallback) so a renamed/moved file keeps its history.
//
// Phase-1 schema is intentionally narrow: aggregate counters plus first/last
// timestamps. Day-bucket history for sparklines/heatmaps lands in phase 2.
struct BookReadingStats {
  std::string docId;
  std::string title;
  std::string author;
  uint32_t totalSeconds = 0;      // idle-clamped, sum across all sessions
  uint32_t pagesTurned = 0;       // forward + backward
  uint32_t sessions = 0;          // session-open count
  // 0 if HalClock was never synced when the session ran. Treat as "unknown".
  time_t firstReadEpoch = 0;
  time_t lastReadEpoch = 0;
  uint8_t progress = 0;           // 0-100, snapshot of last known progress
  bool finished = false;          // user-marked finished (Phase 1: always false)
};

class ReadingStatsStore;
namespace JsonSettingsIO {
bool loadReadingStats(ReadingStatsStore& store, const char* json);
}  // namespace JsonSettingsIO

// Singleton store for per-book + global reading stats.
//
// Persistence model (mirrors RecentBooksStore):
//   /.crosspoint/reading-stats.json   — one file, all books + global counters
//
// One file is fine for phase 1: ESP32 memory + SD seek cost both favour a
// single small JSON over per-book files. We'll split if it ever grows too
// large (likely never — 50 books * ~120 bytes ≈ 6 KB).
class ReadingStatsStore {
  static ReadingStatsStore instance;

  std::vector<BookReadingStats> books;

  // Global aggregates — sum across all books.
  uint32_t globalTotalSeconds = 0;
  uint32_t globalTotalSessions = 0;
  uint32_t globalTotalPagesTurned = 0;

  friend bool JsonSettingsIO::loadReadingStats(ReadingStatsStore&, const char*);

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  // Apply a finished session to the store. Creates a per-book entry on first
  // use. Increments aggregate counters. Updates first/last epoch when
  // walltimeEpoch != 0 (HalClock was synced). Caller is responsible for
  // calling saveToFile() — we don't auto-persist on every page turn.
  void recordSession(const std::string& docId, const std::string& title, const std::string& author,
                     uint32_t sessionSeconds, uint32_t sessionPagesTurned, uint8_t progress, time_t walltimeEpoch);

  // Lookup by document hash; returns nullptr if unknown.
  const BookReadingStats* findBook(const std::string& docId) const;

  const std::vector<BookReadingStats>& getBooks() const { return books; }
  uint32_t getGlobalTotalSeconds() const { return globalTotalSeconds; }
  uint32_t getGlobalTotalSessions() const { return globalTotalSessions; }
  uint32_t getGlobalTotalPagesTurned() const { return globalTotalPagesTurned; }
  size_t getBookCount() const { return books.size(); }

  bool saveToFile() const;
  bool loadFromFile();
};

#define READING_STATS ReadingStatsStore::getInstance()

#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// Day buckets are keyed by an ordinal day count (days since 1970-01-01 in
// LOCAL time, computed by localDayIndex() below). A "reading day" is the
// calendar day the session ENDED in — phase 2 keeps this simple and doesn't
// model the KOReader "day shift" / hour cutoff setting yet.
struct DayBucket {
  uint16_t dayIndex = 0;
  uint32_t seconds = 0;
};

// Helpers — both return 0 when HalClock is unsynced (caller should skip).
uint16_t localDayIndexFromEpoch(time_t epoch);
uint16_t currentLocalDayIndex();

// Per-book reading statistics. Keyed by KOReader document hash (or filename
// hash fallback) so a renamed/moved file keeps its history.
struct BookReadingStats {
  std::string docId;
  std::string title;
  std::string author;
  uint32_t totalSeconds = 0;  // idle-clamped, sum across all sessions
  uint32_t pagesTurned = 0;   // forward + backward
  uint32_t sessions = 0;      // session-open count
  // 0 if HalClock was never synced when the session ran. Treat as "unknown".
  time_t firstReadEpoch = 0;
  time_t lastReadEpoch = 0;
  uint8_t progress = 0;          // 0-100, snapshot of last known progress
  uint16_t finishedCount = 0;    // number of times the user has marked it finished
  time_t lastFinishedEpoch = 0;  // wallclock of the most recent finish (0 if unknown)
  // Sparse day buckets, sorted ascending by dayIndex. Only days with reading
  // are stored — the typical case is a few dozen entries. Bucket with
  // dayIndex == 0 is reserved for "clock-unknown" sessions and is excluded
  // from sparklines/streaks but kept so totals remain consistent.
  std::vector<DayBucket> days;
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
  // Global per-day reading time, sorted ascending. Same shape as per-book.
  // Used to compute streaks and the sparkline on the stats screen.
  std::vector<DayBucket> globalDays;

  friend bool JsonSettingsIO::loadReadingStats(ReadingStatsStore&, const char*);

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  // Apply a finished session to the store. Creates a per-book entry on first
  // use. Increments aggregate counters. Updates first/last epoch when
  // walltimeEpoch != 0 (HalClock was synced). When walltimeEpoch != 0, also
  // credits the session into a local-day bucket on both the book and the
  // global map. Caller is responsible for calling saveToFile() — we don't
  // auto-persist on every page turn.
  void recordSession(const std::string& docId, const std::string& title, const std::string& author,
                     uint32_t sessionSeconds, uint32_t sessionPagesTurned, uint8_t progress, time_t walltimeEpoch);

  // Mark the given book as having been finished once more. Bumps the per-
  // book counter and last-finished epoch (when walltime is available).
  // Creates the per-book entry on demand for books that have been opened
  // but never accumulated a session — e.g. a quick "mark as read" from the
  // menu before any reading time was recorded.
  void markFinished(const std::string& docId, const std::string& title, const std::string& author,
                    time_t walltimeEpoch);

  // Lookup by document hash; returns nullptr if unknown.
  const BookReadingStats* findBook(const std::string& docId) const;

  // ---- Reading speed / time-to-finish estimates -----------------------------
  //
  // Average seconds spent reading per 1% of book progress. We compute this
  // from a book's own history when it has covered enough ground to be
  // statistically meaningful, otherwise fall back to the global average over
  // all books. Returns 0 when no usable signal exists yet (caller should
  // render the ETA as "—").
  //
  // The minimum-progress gate prevents wildly optimistic estimates from books
  // the user has barely opened (e.g. 30 seconds of reading at 2% progress
  // shouldn't extrapolate to a 25-minute book).
  static constexpr uint8_t MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE = 3;  // %
  static constexpr uint32_t MIN_GLOBAL_SECONDS_FOR_RATE = 60;        // s
  float avgSecondsPerPercent(const std::string& docId) const;
  float globalAvgSecondsPerPercent() const;

  // ETA in seconds for finishing `remainingPercent` (0..100) of a book.
  // Uses the per-book rate when available, else the global average. Returns
  // 0 when no rate is available or when remainingPercent <= 0.
  uint32_t estimateRemainingSeconds(const std::string& docId, float remainingPercent) const;

  const std::vector<BookReadingStats>& getBooks() const { return books; }
  uint32_t getGlobalTotalSeconds() const { return globalTotalSeconds; }
  uint32_t getGlobalTotalSessions() const { return globalTotalSessions; }
  uint32_t getGlobalTotalPagesTurned() const { return globalTotalPagesTurned; }
  size_t getBookCount() const { return books.size(); }
  // Count of distinct books that have been finished at least once. Derived
  // on read so we don't need a separate aggregate counter to keep in sync.
  size_t getFinishedBookCount() const;

  // Read-only view of the global day map.
  const std::vector<DayBucket>& getGlobalDays() const { return globalDays; }

  // Seconds read on a specific local-day index. 0 if unknown.
  uint32_t getSecondsForDay(uint16_t dayIndex) const;

  // Current streak in days, ending at `today` (or `today-1` for a 1-day grace
  // so a session that ended just past midnight still extends yesterday's
  // streak when you check this morning). Returns 0 if globalDays is empty or
  // if HalClock is unsynced (so `today == 0`).
  uint16_t computeCurrentStreak(uint16_t today) const;

  // Longest run of consecutive days with any reading.
  uint16_t computeLongestStreak() const;

  bool saveToFile() const;
  bool loadFromFile();
};

#define READING_STATS ReadingStatsStore::getInstance()

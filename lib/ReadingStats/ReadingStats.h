#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace reading_stats {

// Per-book accumulated reading statistics. Time is wall-clock-free: it is
// derived from monotonic millis() deltas, so it works identically on X3 and
// X4 (neither exposes a reliable calendar date).
struct BookStats {
  std::string bookPath;         // identity key (epub->getPath())
  uint32_t pagesRead = 0;       // forward page turns only
  uint32_t totalReadingMs = 0;  // summed, with each gap capped (see kMaxPageMs)
  uint32_t sessionCount = 0;    // number of ended reading sessions

  bool operator==(const BookStats& o) const {
    return bookPath == o.bookPath && pagesRead == o.pagesRead && totalReadingMs == o.totalReadingMs &&
           sessionCount == o.sessionCount;
  }
};

// Pure aggregation engine. No Arduino/SD dependencies, so it is unit-tested on
// the host. Persistence lives in ReadingStatsStore (src/), mirroring
// RecentBooksStore. All timestamps are millis() values (monotonic, ms).
class ReadingStatsAggregator {
 public:
  // The time attributed to a single page turn is capped at this value, so a
  // device left on an open page does not inflate reading time. A gap longer
  // than this still counts, but only up to the cap.
  static constexpr uint32_t kMaxPageMs = 300000;  // 5 minutes

  // Replace all per-book stats with previously persisted data.
  void load(std::vector<BookStats> books);

  // Snapshot for persistence.
  const std::vector<BookStats>& books() const { return books_; }

  // Begin a reading session for a book. If a session is already active it is
  // ended first (banked at nowMs) before the new one starts.
  void beginSession(const std::string& bookPath, uint32_t nowMs);

  // Record a page turn within the active session. `forward` true counts the
  // page as read; either direction banks elapsed time. No-op without a session.
  void recordPageTurn(uint32_t nowMs, bool forward);

  // End the active session, banking the final delta and counting the session.
  // No-op if no session is active.
  void endSession(uint32_t nowMs);

  // Aggregate accessors.
  const BookStats* statsFor(const std::string& bookPath) const;
  uint32_t totalPagesRead() const;
  uint32_t totalReadingMs() const;

  // Reading speed for one book in pages per hour (0 if no time recorded).
  uint32_t pagesPerHour(const std::string& bookPath) const;

 private:
  // Wrap-safe, capped delta from lastEventMs_ to nowMs.
  uint32_t cappedDelta(uint32_t nowMs) const;

  std::vector<BookStats> books_;
  bool sessionActive_ = false;
  int activeIndex_ = -1;  // index into books_ for the active session
  uint32_t lastEventMs_ = 0;
};

}  // namespace reading_stats

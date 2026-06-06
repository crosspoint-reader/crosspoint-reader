#include "ReadingStats.h"

#include <algorithm>

namespace reading_stats {

void ReadingStatsAggregator::load(std::vector<BookStats> books) {
  books_ = std::move(books);
  sessionActive_ = false;
  activeIndex_.reset();
  lastEventMs_ = 0;
}

uint32_t ReadingStatsAggregator::cappedDelta(uint32_t nowMs) const {
  // millis() wraps roughly every 49.7 days; treat a backwards jump as no time.
  if (nowMs < lastEventMs_) return 0;
  const uint32_t delta = nowMs - lastEventMs_;
  return std::min(delta, kMaxPageMs);
}

void ReadingStatsAggregator::beginSession(const std::string&, uint32_t) {
  // Implemented in Task 2.
}

void ReadingStatsAggregator::recordPageTurn(uint32_t, bool) {
  // Implemented in Task 2.
}

void ReadingStatsAggregator::endSession(uint32_t) {
  // Implemented in Task 2.
}

const BookStats* ReadingStatsAggregator::statsFor(const std::string& bookPath) const {
  auto it = std::find_if(books_.begin(), books_.end(),
                         [&](const BookStats& b) { return b.bookPath == bookPath; });
  return it == books_.end() ? nullptr : &*it;
}

uint32_t ReadingStatsAggregator::totalPagesRead() const {
  uint64_t total = 0;
  for (const auto& b : books_) total += b.pagesRead;
  return total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
}

uint32_t ReadingStatsAggregator::totalReadingMs() const {
  uint64_t total = 0;
  for (const auto& b : books_) total += b.totalReadingMs;
  return total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
}

uint32_t ReadingStatsAggregator::pagesPerHour(const std::string& bookPath) const {
  const BookStats* s = statsFor(bookPath);
  if (!s || s->totalReadingMs == 0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(s->pagesRead) * 3600000ULL / s->totalReadingMs);
}

}  // namespace reading_stats

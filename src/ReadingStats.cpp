#include "ReadingStats.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <ctime>

namespace {
constexpr char READING_STATS_FILE[] = "/.crosspoint/reading-stats.json";

// Add `seconds` to the bucket for `dayIndex` in `days`, inserting in sorted
// position if absent. dayIndex == 0 ("unknown day") is silently skipped here —
// the caller decides whether to credit unknown-day reading to a sentinel
// bucket or drop it entirely.
void mergeDay(std::vector<DayBucket>& days, uint16_t dayIndex, uint32_t seconds) {
  if (dayIndex == 0 || seconds == 0) return;
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != days.end() && it->dayIndex == dayIndex) {
    it->seconds += seconds;
  } else {
    days.insert(it, {dayIndex, seconds});
  }
}

uint16_t dayIndexFromLocaltime(const struct tm& t) {
  // Days since 1970-01-01 by Y/M/D in local time. Uses the proleptic
  // Gregorian calendar — close enough for a 65k-day uint16 range (≈179
  // years). We deliberately do NOT call mktime() to avoid DST round-trip
  // surprises near transition midnights.
  const int year = t.tm_year + 1900;
  const int month = t.tm_mon + 1;
  const int day = t.tm_mday;
  // Howard Hinnant's days-from-civil, lightly inlined.
  const int y = year - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + static_cast<long>(doe) - 719468L;
  if (days < 1 || days > 65535) return 0;  // outside our uint16_t window
  return static_cast<uint16_t>(days);
}

}  // namespace

uint16_t localDayIndexFromEpoch(time_t epoch) {
  if (epoch == 0) return 0;
  struct tm t{};
  localtime_r(&epoch, &t);
  return dayIndexFromLocaltime(t);
}

uint16_t currentLocalDayIndex() {
  if (!HalClock::isSynced()) return 0;
  return localDayIndexFromEpoch(HalClock::now());
}

ReadingStatsStore ReadingStatsStore::instance;

void ReadingStatsStore::recordSession(const std::string& docId, const std::string& title, const std::string& author,
                                      uint32_t sessionSeconds, uint32_t sessionPagesTurned, uint8_t progress,
                                      time_t walltimeEpoch) {
  if (docId.empty() || sessionSeconds == 0) {
    // Nothing to credit. Title-update-only flows go through a different path.
    return;
  }

  auto it = std::find_if(books.begin(), books.end(), [&docId](const BookReadingStats& b) { return b.docId == docId; });
  if (it == books.end()) {
    BookReadingStats fresh;
    fresh.docId = docId;
    fresh.title = title;
    fresh.author = author;
    books.push_back(std::move(fresh));
    it = books.end() - 1;
  } else {
    // Update title/author opportunistically — they may have been blank if the
    // book was first opened before metadata was indexed.
    if (!title.empty()) it->title = title;
    if (!author.empty()) it->author = author;
  }

  it->totalSeconds += sessionSeconds;
  it->pagesTurned += sessionPagesTurned;
  it->sessions += 1;
  it->progress = progress;
  if (walltimeEpoch != 0) {
    if (it->firstReadEpoch == 0) it->firstReadEpoch = walltimeEpoch;
    it->lastReadEpoch = walltimeEpoch;
    // Credit the local-day buckets on both the book and the global map.
    // We only do this when the clock is trustworthy; unknown-day sessions
    // still contribute to the running totals above but not to streaks.
    const uint16_t day = localDayIndexFromEpoch(walltimeEpoch);
    mergeDay(it->days, day, sessionSeconds);
    mergeDay(globalDays, day, sessionSeconds);
  }

  globalTotalSeconds += sessionSeconds;
  globalTotalSessions += 1;
  globalTotalPagesTurned += sessionPagesTurned;
}

uint32_t ReadingStatsStore::getSecondsForDay(uint16_t dayIndex) const {
  if (dayIndex == 0) return 0;
  auto it = std::lower_bound(globalDays.begin(), globalDays.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != globalDays.end() && it->dayIndex == dayIndex) return it->seconds;
  return 0;
}

uint16_t ReadingStatsStore::computeCurrentStreak(uint16_t today) const {
  if (today == 0 || globalDays.empty()) return 0;
  // 1-day grace: if there's no reading today, the streak may still end at
  // yesterday. After that the chain is broken.
  uint16_t anchor = today;
  if (getSecondsForDay(anchor) == 0) {
    anchor -= 1;
    if (getSecondsForDay(anchor) == 0) return 0;
  }
  uint16_t streak = 0;
  while (anchor > 0 && getSecondsForDay(anchor) > 0) {
    streak += 1;
    if (anchor == 1) break;
    anchor -= 1;
  }
  return streak;
}

uint16_t ReadingStatsStore::computeLongestStreak() const {
  if (globalDays.empty()) return 0;
  uint16_t longest = 1;
  uint16_t run = 1;
  for (size_t i = 1; i < globalDays.size(); ++i) {
    if (globalDays[i].dayIndex == globalDays[i - 1].dayIndex + 1) {
      run += 1;
      if (run > longest) longest = run;
    } else {
      run = 1;
    }
  }
  return longest;
}

void ReadingStatsStore::markFinished(const std::string& docId, const std::string& title, const std::string& author,
                                     time_t walltimeEpoch) {
  if (docId.empty()) return;
  auto it = std::find_if(books.begin(), books.end(), [&docId](const BookReadingStats& b) { return b.docId == docId; });
  if (it == books.end()) {
    BookReadingStats fresh;
    fresh.docId = docId;
    fresh.title = title;
    fresh.author = author;
    books.push_back(std::move(fresh));
    it = books.end() - 1;
  } else {
    if (!title.empty()) it->title = title;
    if (!author.empty()) it->author = author;
  }
  it->finishedCount += 1;
  it->progress = 100;
  if (walltimeEpoch != 0) {
    it->lastFinishedEpoch = walltimeEpoch;
    if (it->lastReadEpoch < walltimeEpoch) it->lastReadEpoch = walltimeEpoch;
  }
}

size_t ReadingStatsStore::getFinishedBookCount() const {
  return static_cast<size_t>(
      std::count_if(books.begin(), books.end(), [](const BookReadingStats& b) { return b.finishedCount > 0; }));
}

const BookReadingStats* ReadingStatsStore::findBook(const std::string& docId) const {
  auto it = std::find_if(books.begin(), books.end(), [&docId](const BookReadingStats& b) { return b.docId == docId; });
  return it == books.end() ? nullptr : &*it;
}

float ReadingStatsStore::globalAvgSecondsPerPercent() const {
  if (globalTotalSeconds < MIN_GLOBAL_SECONDS_FOR_RATE) return 0.0f;
  // Average over books that have actual progress recorded. A book at 0%
  // contributes time but no progress denominator and would skew the rate
  // toward infinity. Books with progress >= MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE
  // are considered "real readings" for the purpose of the global average.
  uint32_t totalProgressPercents = 0;
  uint32_t totalSecondsFromCountedBooks = 0;
  for (const auto& b : books) {
    if (b.progress < MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE) continue;
    totalProgressPercents += b.progress;
    totalSecondsFromCountedBooks += b.totalSeconds;
  }
  if (totalProgressPercents == 0 || totalSecondsFromCountedBooks == 0) return 0.0f;
  return static_cast<float>(totalSecondsFromCountedBooks) / static_cast<float>(totalProgressPercents);
}

float ReadingStatsStore::avgSecondsPerPercent(const std::string& docId) const {
  const BookReadingStats* b = findBook(docId);
  if (b && b->progress >= MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE && b->totalSeconds > 0) {
    return static_cast<float>(b->totalSeconds) / static_cast<float>(b->progress);
  }
  return globalAvgSecondsPerPercent();
}

uint32_t ReadingStatsStore::estimateRemainingSeconds(const std::string& docId, float remainingPercent) const {
  if (remainingPercent <= 0.0f) return 0;
  if (remainingPercent > 100.0f) remainingPercent = 100.0f;
  const float rate = avgSecondsPerPercent(docId);
  if (rate <= 0.0f) return 0;
  return static_cast<uint32_t>(remainingPercent * rate + 0.5f);
}

bool ReadingStatsStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveReadingStats(*this, READING_STATS_FILE);
}

bool ReadingStatsStore::loadFromFile() {
  if (!Storage.exists(READING_STATS_FILE)) {
    return false;
  }
  String json = Storage.readFile(READING_STATS_FILE);
  if (json.isEmpty()) {
    return false;
  }
  return JsonSettingsIO::loadReadingStats(*this, json.c_str());
}

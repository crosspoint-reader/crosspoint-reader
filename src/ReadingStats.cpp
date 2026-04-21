#include "ReadingStats.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>
#include <ctime>

#include "BookStats.h"
#include "KoInsightPendingSessions.h"

namespace {
constexpr uint8_t STATS_FILE_VERSION = 2;
constexpr char STATS_FILE[] = "/.crosspoint/reading_stats.bin";
}  // namespace

ReadingStats ReadingStats::instance;

void ReadingStats::startSession() {
  sessionStartTime = time(nullptr);
  sessionStartMillis = millis();
  sessionActive = true;
}

uint32_t ReadingStats::currentSessionElapsedSeconds() const {
  if (!sessionActive) return 0;
  constexpr time_t MIN_VALID_TIME = 1704067200;  // 2024-01-01 UTC
  const time_t now = time(nullptr);
  if (sessionStartTime >= MIN_VALID_TIME && now >= MIN_VALID_TIME) {
    const int64_t diff = static_cast<int64_t>(now) - static_cast<int64_t>(sessionStartTime);
    if (diff > 0 && diff <= static_cast<int64_t>(86400)) {
      return static_cast<uint32_t>(diff);
    }
  }
  const uint32_t ms = millis() - sessionStartMillis;
  const uint32_t sec = ms / 1000;
  if (sec == 0 || sec > 86400U) return 0;
  return sec;
}

void ReadingStats::endSession(const char* title, uint8_t progress, const char* bookPath, uint16_t pageOneBased,
                              uint16_t chapterTotalPages) {
  if (!sessionActive) return;

  constexpr time_t MIN_VALID_TIME = 1704067200;
  const time_t now = time(nullptr);
  struct tm timeinfo = {};
  localtime_r(&now, &timeinfo);
  const int16_t curYear = static_cast<int16_t>(timeinfo.tm_year);
  const int16_t curDay = static_cast<int16_t>(timeinfo.tm_yday);
  if (now >= MIN_VALID_TIME && (curDay != lastReadDayOfYear || curYear != lastReadYear)) {
    bool isConsecutive = false;
    if (lastReadDayOfYear >= 0) {
      if (curYear == lastReadYear && curDay == lastReadDayOfYear + 1) {
        isConsecutive = true;
      } else if (curYear == lastReadYear + 1 && curDay == 0) {
        isConsecutive = (lastReadDayOfYear == 364 || lastReadDayOfYear == 365);
      }
    }
    if (isConsecutive) {
      currentStreak++;
    } else {
      currentStreak = 1;
    }
    if (currentStreak > longestStreak) longestStreak = currentStreak;

    todayReadSeconds = 0;
    lastReadYear = curYear;
    lastReadDayOfYear = curDay;
  }

  constexpr uint32_t MAX_SESSION_SECS = 86400;
  uint32_t elapsedSecs = 0;
  if (sessionStartTime >= MIN_VALID_TIME && now >= MIN_VALID_TIME) {
    const int64_t diff = static_cast<int64_t>(now) - static_cast<int64_t>(sessionStartTime);
    if (diff > 0 && diff < static_cast<int64_t>(MAX_SESSION_SECS)) {
      elapsedSecs = static_cast<uint32_t>(diff);
    }
  }
  if (elapsedSecs == 0 && sessionStartMillis != 0) {
    const uint32_t ms = millis() - sessionStartMillis;
    const uint32_t sec = ms / 1000;
    if (sec > 0 && sec < MAX_SESSION_SECS) {
      elapsedSecs = sec;
    }
  }

  if (elapsedSecs > 0) {
    todayReadSeconds += elapsedSecs;
    totalReadSeconds += elapsedSecs;
    totalSessions++;
  }

  const uint8_t prevProgress = lastBookProgress;
  if (title && title[0] != '\0') {
    strncpy(lastBookTitle, title, sizeof(lastBookTitle) - 1);
    lastBookTitle[sizeof(lastBookTitle) - 1] = '\0';
  }
  lastBookProgress = progress;
  if (progress >= 100 && prevProgress < 100) {
    booksFinished++;
  }

  sessionActive = false;
  sessionStartMillis = 0;
  saveToFile();

  if (bookPath && bookPath[0] != '\0' && elapsedSecs > 0) {
    BOOK_STATS.updateBook(bookPath, title, elapsedSecs, progress);
    int64_t startTs = static_cast<int64_t>(sessionStartTime);
    if (sessionStartTime < MIN_VALID_TIME || now < MIN_VALID_TIME) {
      startTs = static_cast<int64_t>(now) - static_cast<int64_t>(elapsedSecs);
    }
    if (startTs < 0) {
      startTs = static_cast<int64_t>(now);
    }
    KoInsightPendingSessions::append(bookPath, startTs, elapsedSecs, pageOneBased, chapterTotalPages);
  }
}

bool ReadingStats::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  FsFile file;
  if (!Storage.openFileForWrite("RST", STATS_FILE, file)) {
    LOG_ERR("RST", "Failed to open reading_stats.bin for write");
    return false;
  }
  const uint8_t version = STATS_FILE_VERSION;
  serialization::writePod(file, version);
  serialization::writePod(file, totalReadSeconds);
  serialization::writePod(file, todayReadSeconds);
  serialization::writePod(file, lastReadYear);
  serialization::writePod(file, lastReadDayOfYear);
  serialization::writePod(file, lastBookProgress);
  file.write(reinterpret_cast<const uint8_t*>(lastBookTitle), sizeof(lastBookTitle));
  serialization::writePod(file, totalSessions);
  serialization::writePod(file, booksFinished);
  serialization::writePod(file, currentStreak);
  serialization::writePod(file, longestStreak);
  file.close();
  return true;
}

bool ReadingStats::loadFromFile() {
  FsFile file;
  if (!Storage.openFileForRead("RST", STATS_FILE, file)) {
    BOOK_STATS.loadFromFile();
    return false;
  }
  uint8_t version;
  serialization::readPod(file, version);
  if (version != 1 && version != STATS_FILE_VERSION) {
    LOG_ERR("RST", "Unknown reading_stats.bin version %u", version);
    file.close();
    return false;
  }
  serialization::readPod(file, totalReadSeconds);
  serialization::readPod(file, todayReadSeconds);
  serialization::readPod(file, lastReadYear);
  serialization::readPod(file, lastReadDayOfYear);
  serialization::readPod(file, lastBookProgress);
  file.read(reinterpret_cast<uint8_t*>(lastBookTitle), sizeof(lastBookTitle));
  lastBookTitle[sizeof(lastBookTitle) - 1] = '\0';
  if (version >= 2) {
    serialization::readPod(file, totalSessions);
    serialization::readPod(file, booksFinished);
    serialization::readPod(file, currentStreak);
    serialization::readPod(file, longestStreak);
  }
  file.close();
  if (version < STATS_FILE_VERSION) saveToFile();

  BOOK_STATS.loadFromFile();
  return true;
}

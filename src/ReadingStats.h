#pragma once
#include <cstdint>
#include <ctime>

// Tracks reading session time and book progress (CrossPet-derived).
// Used for KoInsight and future UI; persists to /.crosspoint/reading_stats.bin
class ReadingStats {
  static ReadingStats instance;
  time_t sessionStartTime = 0;
  uint32_t sessionStartMillis = 0;
  bool sessionActive = false;

 public:
  uint32_t totalReadSeconds = 0;
  uint32_t todayReadSeconds = 0;
  int16_t lastReadYear = 0;
  int16_t lastReadDayOfYear = -1;
  char lastBookTitle[64] = {};
  uint8_t lastBookProgress = 0;

  uint16_t totalSessions = 0;
  uint16_t booksFinished = 0;
  uint16_t currentStreak = 0;
  uint16_t longestStreak = 0;

  static ReadingStats& getInstance() { return instance; }

  void startSession();

  void endSession(const char* title, uint8_t progress, const char* bookPath = nullptr, uint16_t pageOneBased = 0,
                  uint16_t chapterTotalPages = 0);

  bool isSessionActive() const { return sessionActive; }
  time_t sessionStartWallTime() const { return sessionStartTime; }
  /** Elapsed seconds for the current open reader session (0 if inactive or clock invalid). */
  uint32_t currentSessionElapsedSeconds() const;

  bool saveToFile() const;
  bool loadFromFile();
};

#define READ_STATS ReadingStats::getInstance()

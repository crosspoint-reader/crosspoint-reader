#pragma once
#include <cstdint>
#include <string>

struct BookStats {
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesRead = 0;
  uint32_t totalWordsRead = 0;
  uint16_t sessionsCount = 0;
  bool finished = false;

  bool loadFromFile(const std::string& path);
  bool saveToFile(const std::string& path) const;
};

struct GlobalStats {
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesRead = 0;
  uint32_t totalWordsRead = 0;
  uint16_t booksStarted = 0;
  uint16_t booksFinished = 0;

  static constexpr char PATH[] = "/.crosspoint/stats.json";

  bool loadFromFile();
  bool saveToFile() const;
};

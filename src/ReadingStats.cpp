#include "ReadingStats.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

bool BookStats::loadFromFile(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }
  const String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    LOG_ERR("BST", "JSON parse error for %s", path.c_str());
    return false;
  }
  totalReadingSeconds = doc["totalReadingSeconds"] | (uint32_t)0;
  totalPagesRead = doc["totalPagesRead"] | (uint32_t)0;
  totalWordsRead = doc["totalWordsRead"] | (uint32_t)0;
  sessionsCount = doc["sessionsCount"] | (uint16_t)0;
  finished = doc["finished"] | false;
  return true;
}

bool BookStats::saveToFile(const std::string& path) const {
  JsonDocument doc;
  doc["totalReadingSeconds"] = totalReadingSeconds;
  doc["totalPagesRead"] = totalPagesRead;
  doc["totalWordsRead"] = totalWordsRead;
  doc["sessionsCount"] = sessionsCount;
  doc["finished"] = finished;
  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path.c_str(), json);
}

bool GlobalStats::loadFromFile() {
  if (!Storage.exists(PATH)) {
    return false;
  }
  const String json = Storage.readFile(PATH);
  if (json.isEmpty()) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    LOG_ERR("GST", "JSON parse error for global stats");
    return false;
  }
  totalReadingSeconds = doc["totalReadingSeconds"] | (uint32_t)0;
  totalPagesRead = doc["totalPagesRead"] | (uint32_t)0;
  totalWordsRead = doc["totalWordsRead"] | (uint32_t)0;
  booksStarted = doc["booksStarted"] | (uint16_t)0;
  booksFinished = doc["booksFinished"] | (uint16_t)0;
  return true;
}

bool GlobalStats::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["totalReadingSeconds"] = totalReadingSeconds;
  doc["totalPagesRead"] = totalPagesRead;
  doc["totalWordsRead"] = totalWordsRead;
  doc["booksStarted"] = booksStarted;
  doc["booksFinished"] = booksFinished;
  String json;
  serializeJson(doc, json);
  return Storage.writeFile(PATH, json);
}

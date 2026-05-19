#include "ReadingStats.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>

namespace {
constexpr char READING_STATS_FILE[] = "/.crosspoint/reading-stats.json";
}  // namespace

ReadingStatsStore ReadingStatsStore::instance;

void ReadingStatsStore::recordSession(const std::string& docId, const std::string& title, const std::string& author,
                                      uint32_t sessionSeconds, uint32_t sessionPagesTurned, uint8_t progress,
                                      time_t walltimeEpoch) {
  if (docId.empty() || sessionSeconds == 0) {
    // Nothing to credit. Title-update-only flows go through a different path.
    return;
  }

  auto it = std::find_if(books.begin(), books.end(),
                         [&docId](const BookReadingStats& b) { return b.docId == docId; });
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
  }

  globalTotalSeconds += sessionSeconds;
  globalTotalSessions += 1;
  globalTotalPagesTurned += sessionPagesTurned;
}

const BookReadingStats* ReadingStatsStore::findBook(const std::string& docId) const {
  auto it = std::find_if(books.begin(), books.end(),
                         [&docId](const BookReadingStats& b) { return b.docId == docId; });
  return it == books.end() ? nullptr : &*it;
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

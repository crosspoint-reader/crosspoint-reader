#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>

#include <ctime>

namespace readingstats {
namespace {

constexpr char STATS_DIR[] = "/.crosspoint/stats";
constexpr char TOTALS_PATH[] = "/.crosspoint/stats/totals.bin";
constexpr int64_t MIN_VALID_EPOCH = 946684800;  // 2000-01-01

std::string bookPathFor(const std::string& bookPath) {
  const std::string id = KOReaderDocumentId::calculateFromFilename(bookPath);
  return id.empty() ? std::string() : std::string(STATS_DIR) + "/" + id + ".bin";
}

template <typename Record, size_t N>
bool readOne(const std::string& path, Record& out) {
  HalFile file;
  if (!Storage.exists(path.c_str()) || !Storage.openFileForRead("STAT", path, file)) return false;
  uint8_t data[N];
  const int got = file.read(data, N);
  return got == static_cast<int>(N) && file.available() == 0 && decode(data, N, out);
}

// The .tmp is complete whenever it outlives a replace, so it is a valid fallback.
template <typename Record, size_t N>
Record readRecord(const std::string& path) {
  Record record;
  if (!readOne<Record, N>(path, record)) readOne<Record, N>(path + ".tmp", record);
  return record;
}

template <size_t N>
bool writeRecord(const std::string& path, const uint8_t (&data)[N]) {
  if (!Storage.ensureDirectoryExists(STATS_DIR)) {
    LOG_ERR("STAT", "Cannot create %s", STATS_DIR);
    return false;
  }
  const std::string tmp = path + ".tmp";
  {
    HalFile file;
    if (!Storage.openFileForWrite("STAT", tmp, file)) return false;
    const bool complete = file.write(data, N) == N;
    if (!file.close() || !complete) {
      Storage.remove(tmp.c_str());
      LOG_ERR("STAT", "Short write: %s", tmp.c_str());
      return false;
    }
  }
  if (!Storage.replaceFile(tmp.c_str(), path.c_str())) {
    LOG_ERR("STAT", "Cannot replace %s", path.c_str());
    return false;
  }
  return true;
}

bool saveTotals(const Totals& totals) {
  uint8_t data[TOTALS_BYTES];
  encode(totals, data);
  return writeRecord(TOTALS_PATH, data);
}

bool saveBook(const std::string& path, const Book& book) {
  uint8_t data[BOOK_BYTES];
  encode(book, data);
  return writeRecord(path, data);
}

}  // namespace

Totals loadTotals() { return readRecord<Totals, TOTALS_BYTES>(TOTALS_PATH); }

Book loadBook(const std::string& bookPath) {
  const std::string path = bookPathFor(bookPath);
  return path.empty() ? Book{} : readRecord<Book, BOOK_BYTES>(path);
}

void recordSession(const std::string& bookPath, const uint32_t seconds, const uint32_t pages,
                   const int64_t startEpoch) {
  const std::string path = bookPathFor(bookPath);
  if (path.empty() || (seconds == 0 && pages == 0)) return;
  Totals totals = loadTotals();
  Book book = readRecord<Book, BOOK_BYTES>(path);
  applySession(totals, book, seconds, pages, localDay(startEpoch));
  // If the second write fails, the totals undercount this one session.
  if (!saveBook(path, book) || !saveTotals(totals)) LOG_ERR("STAT", "Reading session not fully saved");
}

bool setBookFinished(const std::string& bookPath, const bool finished) {
  const std::string path = bookPathFor(bookPath);
  if (path.empty()) return false;
  Totals totals = loadTotals();
  Book book = readRecord<Book, BOOK_BYTES>(path);
  if (!setFinished(totals, book, finished)) return true;
  if (!saveBook(path, book)) return false;
  if (saveTotals(totals)) return true;
  // Undo the book so its state and the finished-books total stay in step.
  book.finished = !finished;
  saveBook(path, book);
  return false;
}

int32_t localDay(const int64_t epoch) {
  if (epoch < MIN_VALID_EPOCH) return 0;
  const time_t at = static_cast<time_t>(epoch);
  struct tm local;
  if (!localtime_r(&at, &local)) return 0;
  return civilDay(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

}  // namespace readingstats

#include "ReadingStats.h"

#include <Arduino.h>  // millis()
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <ctime>

#include "ReadingStatsJson.h"
#include "ReadingStatsSlotFile.h"

namespace {
// Where a history that cannot be read is set aside: reading-stats.bin -> reading-stats.corrupt.bin,
// reading-stats.json -> reading-stats.corrupt.json.
std::string asidePathFor(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + ".corrupt";
  return path.substr(0, dot) + ".corrupt" + path.substr(dot);
}

std::string parentDirOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

// Add `seconds` to the bucket for `dayIndex` in `days`, inserting in sorted
// position if absent. dayIndex == 0 ("unknown day") is silently skipped here —
// the caller decides whether to credit unknown-day reading to a sentinel
// bucket or drop it entirely.
void mergeDay(std::vector<DayBucket>& days, uint16_t dayIndex, uint32_t seconds, const size_t maxDays) {
  if (dayIndex == 0 || seconds == 0) return;
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != days.end() && it->dayIndex == dayIndex) {
    it->seconds += seconds;
  } else {
    days.insert(it, {dayIndex, seconds});
  }
  // Sorted ascending, so the oldest buckets are at the front.
  if (days.size() > maxDays) days.erase(days.begin(), days.begin() + static_cast<long>(days.size() - maxDays));
}

// Take `seconds` back out of the bucket for `dayIndex`, dropping the bucket once it is empty: an
// empty bucket would still count as a reading day in the longest-streak walk.
void unmergeDay(std::vector<DayBucket>& days, const uint16_t dayIndex, const uint32_t seconds) {
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it == days.end() || it->dayIndex != dayIndex) return;
  if (it->seconds > seconds) {
    it->seconds -= seconds;
  } else {
    days.erase(it);
  }
}

// Length of the run of consecutive reading days that ends on `day`, from a sorted day map.
uint16_t runEndingAt(const std::vector<DayBucket>& days, const uint16_t day) {
  auto it =
      std::lower_bound(days.begin(), days.end(), day, [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it == days.end() || it->dayIndex != day) return 0;
  uint16_t run = 1;
  while (it != days.begin()) {
    const auto prev = it - 1;
    if (prev->dayIndex + 1 != it->dayIndex) break;
    ++run;
    it = prev;
  }
  return run;
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

// ---- The arithmetic ---------------------------------------------------------------------------

uint32_t ReadingStatsStore::secondsOn(const std::vector<DayBucket>& days, const uint16_t dayIndex) {
  if (dayIndex == 0) return 0;
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != days.end() && it->dayIndex == dayIndex) return it->seconds;
  return 0;
}

uint16_t ReadingStatsStore::currentStreakIn(const std::vector<DayBucket>& days, const uint16_t today) {
  if (today == 0 || days.empty()) return 0;
  // 1-day grace: if there's no reading today, the streak may still end at
  // yesterday. After that the chain is broken.
  uint16_t anchor = today;
  if (secondsOn(days, anchor) == 0) {
    anchor -= 1;
    if (secondsOn(days, anchor) == 0) return 0;
  }
  uint16_t streak = 0;
  while (anchor > 0 && secondsOn(days, anchor) > 0) {
    streak += 1;
    if (anchor == 1) break;
    anchor -= 1;
  }
  return streak;
}

uint16_t ReadingStatsStore::longestStreakIn(const std::vector<DayBucket>& days, const uint16_t record) {
  if (days.empty()) return record;
  uint16_t longest = std::max<uint16_t>(1, record);
  uint16_t run = 1;
  for (size_t i = 1; i < days.size(); ++i) {
    if (days[i].dayIndex == days[i - 1].dayIndex + 1) {
      run += 1;
      if (run > longest) longest = run;
    } else {
      run = 1;
    }
  }
  return longest;
}

bool ReadingStatsStore::evictsBefore(const time_t aLastRead, const uint32_t aSeconds, const time_t bLastRead,
                                     const uint32_t bSeconds) {
  if (aLastRead != bLastRead) return aLastRead < bLastRead;
  return aSeconds < bSeconds;
}

float ReadingStatsStore::pooledSecondsPerPercent(const uint32_t globalTotalSeconds, const uint32_t countedSeconds,
                                                 const uint32_t countedPercents) {
  if (globalTotalSeconds < MIN_GLOBAL_SECONDS_FOR_RATE) return 0.0f;
  if (countedPercents == 0 || countedSeconds == 0) return 0.0f;
  return static_cast<float>(countedSeconds) / static_cast<float>(countedPercents);
}

float ReadingStatsStore::ownSecondsPerPercent(const uint32_t totalSeconds, const uint8_t progress) {
  if (!countsTowardPace(progress) || totalSeconds == 0) return 0.0f;
  return static_cast<float>(totalSeconds) / static_cast<float>(progress);
}

uint32_t ReadingStatsStore::etaSeconds(const float secondsPerPercent, float remainingPercent) {
  if (remainingPercent <= 0.0f) return 0;
  if (remainingPercent > 100.0f) remainingPercent = 100.0f;
  if (secondsPerPercent <= 0.0f) return 0;
  return static_cast<uint32_t>(remainingPercent * secondsPerPercent + 0.5f);
}

void ReadingStatsStore::trimBookDays(std::vector<DayBucket>& days) {
  if (days.size() > kMaxBookDays)
    days.erase(days.begin(), days.begin() + static_cast<long>(days.size() - kMaxBookDays));
}

void ReadingStatsStore::trimGlobalDays(std::vector<DayBucket>& days, uint16_t& record) {
  if (days.size() <= kMaxGlobalDays) return;
  // The record streak may live in the buckets about to go: measure before trimming.
  record = longestStreakIn(days, record);
  days.erase(days.begin(), days.begin() + static_cast<long>(days.size() - kMaxGlobalDays));
}

void ReadingStatsStore::takeOut(const BookReadingStats& book, ReadingTotals& totals) {
  totals.totalSeconds -= std::min(totals.totalSeconds, book.totalSeconds);
  totals.totalSessions -= std::min(totals.totalSessions, book.sessions);
  totals.totalPagesTurned -= std::min(totals.totalPagesTurned, book.pagesTurned);
  for (const DayBucket& day : book.days) unmergeDay(totals.globalDays, day.dayIndex, day.seconds);
}

void ReadingStatsStore::applySession(BookReadingStats& book, ReadingTotals& totals, const std::string& title,
                                     const std::string& author, const uint32_t sessionSeconds,
                                     const uint32_t sessionPagesTurned, const uint8_t progress,
                                     const time_t walltimeEpoch) {
  if (!title.empty()) book.title = title;
  if (!author.empty()) book.author = author;
  book.totalSeconds += sessionSeconds;
  book.pagesTurned += sessionPagesTurned;
  book.progress = progress;
  if (sessionSeconds > 0) {
    book.sessions += 1;
    totals.totalSessions += 1;
  }
  if (walltimeEpoch != 0) {
    if (book.firstReadEpoch == 0) book.firstReadEpoch = walltimeEpoch;
    book.lastReadEpoch = walltimeEpoch;
    const uint16_t day = localDayIndexFromEpoch(walltimeEpoch);
    mergeDay(book.days, day, sessionSeconds, kMaxBookDays);
    mergeDay(totals.globalDays, day, sessionSeconds, kMaxGlobalDays);
    // Fold this day's run into the persisted longest streak while the whole run is still in the
    // window (a run longer than the window is already the record).
    const uint16_t run = runEndingAt(totals.globalDays, day);
    if (run > totals.longestStreak) totals.longestStreak = run;
  }
  totals.totalSeconds += sessionSeconds;
  totals.totalPagesTurned += sessionPagesTurned;
}

void ReadingStatsStore::applyFinish(BookReadingStats& book, const std::string& title, const std::string& author,
                                    const time_t walltimeEpoch) {
  if (!title.empty()) book.title = title;
  if (!author.empty()) book.author = author;
  book.finishedCount += 1;
  book.progress = 100;
  if (walltimeEpoch != 0) {
    book.lastFinishedEpoch = walltimeEpoch;
    if (book.lastReadEpoch < walltimeEpoch) book.lastReadEpoch = walltimeEpoch;
  }
}

namespace {

using ReadingStatsSlotFile::DocKey;
using ReadingStatsSlotFile::Entry;
using ReadingStatsSlotFile::kEntryCount;
using ReadingStatsSlotFile::kNoCopy;
using ReadingStatsSlotFile::kNoSlot;
using ReadingStatsSlotFile::kSlotSize;
using ReadingStatsSlotFile::Load;
using ReadingStatsSlotFile::Meta;

// Sets an unreadable file aside for forensics, rather than lose it or stall on it for ever.
bool setAside(const std::string& path) {
  const std::string aside = asidePathFor(path);
  Storage.remove(aside.c_str());
  if (!Storage.rename(path.c_str(), aside.c_str())) {
    LOG_ERR("RST", "%s unreadable and could not be set aside; left as is", path.c_str());
    return false;
  }
  LOG_ERR("RST", "%s unreadable; set aside as %s", path.c_str(), aside.c_str());
  return true;
}

// What survives of a book whose slot fails its check: the directory's figures. The title, author and
// days are lost; the book's next update writes a whole slot again.
BookReadingStats bookFromEntry(const Entry& entry) {
  BookReadingStats book;
  book.docId = ReadingStatsSlotFile::formatDocId(entry.key);
  book.totalSeconds = entry.totalSeconds;
  book.progress = entry.progress;
  book.finishedCount = entry.finishedCount;
  book.lastReadEpoch = static_cast<time_t>(entry.lastReadEpoch);
  return book;
}

enum class BookRead : uint8_t { Ok, Damaged, IoError };

// The book an entry names: from its slot, or from the directory's figures when the slot is damaged
// (Damaged) or the card did not give it back (IoError). Queries show either; an update must not
// build on an IoError, which says nothing about the slot.
BookRead readBook(FsFile& file, const Entry& entry, uint8_t* image, BookReadingStats& book) {
  if (!ReadingStatsSlotFile::readSlot(file, entry.slot, image)) {
    LOG_ERR("RST", "Slot %u could not be read", static_cast<unsigned>(entry.slot));
    book = bookFromEntry(entry);
    return BookRead::IoError;
  }
  DocKey key{};
  if (ReadingStatsSlotFile::decodeSlot(image, key, book) && key == entry.key) return BookRead::Ok;
  LOG_ERR("RST", "Slot %u fails its check; %s keeps only its directory figures", static_cast<unsigned>(entry.slot),
          ReadingStatsSlotFile::formatDocId(entry.key).c_str());
  book = bookFromEntry(entry);
  return BookRead::Damaged;
}

// The global pace from the directory alone.
float pooledPaceOf(const Meta& meta) {
  uint32_t seconds = 0;
  uint32_t percents = 0;
  for (size_t i = 0; i < kEntryCount; ++i) {
    const Entry entry = meta.entry(i);
    if (entry.used() && ReadingStatsStore::countsTowardPace(entry.progress)) {
      seconds += entry.totalSeconds;
      percents += entry.progress;
    }
  }
  return ReadingStatsStore::pooledSecondsPerPercent(meta.totalSeconds(), seconds, percents);
}

void summarize(const Meta& meta, ReadingStatsStore::Summary& out, const bool withIndex) {
  meta.readTotals(out);
  out.seq = meta.seq();
  if (withIndex) out.byTime.reserve(meta.bookCount());
  for (size_t i = 0; i < kEntryCount; ++i) {
    const Entry entry = meta.entry(i);
    if (!entry.used()) continue;
    ++out.bookCount;
    if (entry.finishedCount > 0) ++out.finishedBookCount;
    if (ReadingStatsStore::countsTowardPace(entry.progress)) {
      out.paceSeconds += entry.totalSeconds;
      out.pacePercents += entry.progress;
    }
    if (withIndex) out.byTime.push_back({entry.totalSeconds, entry.key});
  }
  std::stable_sort(out.byTime.begin(), out.byTime.end(),
                   [](const ReadingStatsStore::IndexEntry& a, const ReadingStatsStore::IndexEntry& b) {
                     return a.totalSeconds > b.totalSeconds;
                   });
}

// The entry the book cap evicts: the least recently read, ties to less time; the first of equals.
size_t victimOf(const Meta& meta) {
  size_t victim = kEntryCount;
  Entry worst;
  for (size_t i = 0; i < kEntryCount; ++i) {
    const Entry entry = meta.entry(i);
    if (!entry.used()) continue;
    if (victim == kEntryCount ||
        ReadingStatsStore::evictsBefore(static_cast<time_t>(entry.lastReadEpoch), entry.totalSeconds,
                                        static_cast<time_t>(worst.lastReadEpoch), worst.totalSeconds)) {
      victim = i;
      worst = entry;
    }
  }
  return victim;
}

}  // namespace

// ---- Updates ----------------------------------------------------------------------------------

ReadingStatsStore::WriteResult ReadingStatsStore::recordSession(const std::string& docId, const std::string& title,
                                                                const std::string& author,
                                                                const uint32_t sessionSeconds,
                                                                const uint32_t sessionPagesTurned,
                                                                const uint8_t progress, const time_t walltimeEpoch) {
  return write(Edit::Session, docId, [&](BookReadingStats& book, ReadingTotals& totals, const bool existed) {
    if (!existed && sessionSeconds == 0) return false;  // nothing to record
    applySession(book, totals, title, author, sessionSeconds, sessionPagesTurned, progress, walltimeEpoch);
    return true;
  });
}

ReadingStatsStore::WriteResult ReadingStatsStore::markFinished(const std::string& docId, const std::string& title,
                                                               const std::string& author, const time_t walltimeEpoch) {
  return write(Edit::Finish, docId, [&](BookReadingStats& book, ReadingTotals&, bool) {
    applyFinish(book, title, author, walltimeEpoch);
    return true;
  });
}

ReadingStatsStore::WriteResult ReadingStatsStore::removeBook(const std::string& docId) {
  return write(Edit::Remove, docId, [](BookReadingStats& book, ReadingTotals& totals, bool) {
    takeOut(book, totals);
    return true;
  });
}

ReadingStatsStore::WriteResult ReadingStatsStore::renameBook(const std::string& fromDocId, const std::string& toDocId) {
  if (fromDocId == toDocId) return WriteResult::Done;
  return write(
      Edit::Rekey, fromDocId,
      [&](BookReadingStats& book, ReadingTotals&, bool) {
        book.docId = toDocId;
        return true;
      },
      toDocId);
}

ReadingStatsStore::WriteResult ReadingStatsStore::write(
    const Edit edit, const std::string& docId,
    const std::function<bool(BookReadingStats&, ReadingTotals&, bool)>& apply, const std::string& newDocId) {
  // Neither edit can create a book: a removal or a rekey of one the history does not hold is NotFound.
  const bool needsBook = edit == Edit::Remove || edit == Edit::Rekey;
  DocKey key{};
  if (!ReadingStatsSlotFile::parseDocId(docId, key)) {
    // KOReaderDocumentId always gives 32 hex characters; anything else is no book of ours.
    LOG_ERR("RST", "Not a document id: '%s'", docId.c_str());
    return needsBook ? WriteResult::NotFound : WriteResult::Failed;
  }
  // The key the book is written under: its own, or for a rekey the one it moves to.
  DocKey writeKey = key;
  if (edit == Edit::Rekey && !ReadingStatsSlotFile::parseDocId(newDocId, writeKey)) {
    LOG_ERR("RST", "Not a document id: '%s'", newDocId.c_str());
    return WriteResult::Failed;
  }
  [[maybe_unused]] const uint32_t started = millis();
  const ReadResult prepared = prepare();
  if (prepared == ReadResult::NoMemory) return WriteResult::NoMemory;
  if (prepared != ReadResult::Ok) return WriteResult::Failed;
  auto meta = Meta::create();
  auto image = makeUniqueNoThrow<uint8_t[]>(kSlotSize);
  if (!meta || !image) {
    LOG_ERR("RST", "OOM: no room for the meta and a slot; history left as is");
    return WriteResult::NoMemory;
  }

  // 1. The history as it stands, and the book in it.
  uint8_t live = kNoCopy;
  bool fresh = true;  // no readable history: a write that adds reading starts one
  size_t index = kEntryCount;
  BookReadingStats book;
  if (Storage.exists(path_.c_str())) {
    FsFile file;
    if (!Storage.openFileForRead("RST", path_.c_str(), file)) return WriteResult::Failed;
    switch (ReadingStatsSlotFile::loadMeta(file, *meta, live)) {
      case Load::IoError:
        return WriteResult::Failed;
      case Load::Corrupt:
        // A removal or a rekey has nothing to work on in an unreadable file; only a write that adds
        // reading starts a fresh history over it.
        if (needsBook) return WriteResult::Failed;
        break;
      case Load::Ok:
        fresh = false;
        index = meta->find(key);
        // A slot the card would not give back is no reason to rewrite the book from the directory
        // alone: that would drop its days, counts and dates for good. A damaged one is.
        if (index != kEntryCount && readBook(file, meta->entry(index), image.get(), book) == BookRead::IoError) {
          return WriteResult::Failed;
        }
        break;
    }
  }
  if (fresh) meta->clear();
  const bool existed = index != kEntryCount;
  if (!existed && needsBook) return WriteResult::NotFound;
  if (edit == Edit::Rekey && meta->find(writeKey) != kEntryCount) {
    LOG_INF("RST", "%s already has a history; not merging %s into it", newDocId.c_str(), docId.c_str());
    return WriteResult::Failed;
  }
  if (!existed) book.docId = docId;

  // 2. Merge the one book in memory.
  ReadingTotals totals;
  meta->readTotals(totals);
  if (!apply(book, totals, existed)) return WriteResult::Done;

  // 3. A history file to write into.
  if (fresh) {
    if (Storage.exists(path_.c_str()) && !setAside(path_)) return WriteResult::Failed;
    if (!createFresh(*meta)) return WriteResult::Failed;
    live = 0;
  }
  FsFile file;
  if (!Storage.openFileForUpdate("RST", path_.c_str(), file)) return WriteResult::Failed;
  [[maybe_unused]] const uint32_t loaded = millis();

  // 4. Copy-on-write: the book into a slot no entry refers to, then the meta into the copy that is
  // not the live one. Power lost before the meta is whole leaves the live copy, which never
  // referred to that slot: the update is lost, the history is not.
  if (edit == Edit::Remove) {
    meta->setEntry(index, Entry{});
  } else {
    // Chosen while the book's old slot, and the cap's victim's, are still referred to.
    const uint8_t slot = meta->freeSlot();
    if (slot == kNoSlot) {
      LOG_ERR("RST", "No free slot; history left as is");
      return WriteResult::Failed;
    }
    if (!existed) {
      index = meta->freeEntry();
      if (index == kEntryCount) {
        index = victimOf(*meta);
        LOG_INF("RST", "Book cap (%u) reached; dropping the least recently read: %s", static_cast<unsigned>(kMaxBooks),
                ReadingStatsSlotFile::formatDocId(meta->entry(index).key).c_str());
      }
    }
    ReadingStatsSlotFile::encodeSlot(writeKey, book, image.get());
    if (!ReadingStatsSlotFile::writeSlot(file, slot, image.get())) return WriteResult::Failed;
    file.flush();  // on the card before any meta refers to it
    meta->setEntry(index, ReadingStatsSlotFile::entryFor(writeKey, book, slot));
  }
  [[maybe_unused]] const uint32_t slotted = millis();
  if (!meta->writeTotals(totals)) {
    LOG_ERR("RST", "More global days than the file holds; history left as is");
    return WriteResult::Failed;
  }
  meta->setSeq(meta->seq() + 1);
  meta->seal();
  if (!ReadingStatsSlotFile::writeMeta(file, *meta, live == 0 ? 1 : 0)) return WriteResult::Failed;
  file.flush();
  [[maybe_unused]] const uint32_t done = millis();
  LOG_INF("RST", "write done in %lu ms: load %lu, slot %lu, meta %lu (%u books, free=%lu contig=%lu)",
          static_cast<unsigned long>(done - started), static_cast<unsigned long>(loaded - started),
          static_cast<unsigned long>(slotted - loaded), static_cast<unsigned long>(done - slotted),
          static_cast<unsigned>(meta->bookCount()), static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  return WriteResult::Done;
}

// ---- The history file -------------------------------------------------------------------------

ReadingStatsStore::ReadResult ReadingStatsStore::prepare() {
  if (prepared_) return ReadResult::Ok;
  // A create or an import cut short leaves only its temporary file, never half a history.
  const std::string tmpPath = path_ + ".tmp";
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  if (!Storage.exists(path_.c_str()) && Storage.exists(legacyPath_.c_str())) {
    const ReadResult imported = importLegacy();
    if (imported != ReadResult::Ok) return imported;
  }
  prepared_ = true;
  return ReadResult::Ok;
}

ReadingStatsStore::ReadResult ReadingStatsStore::open(FsFile& file, Meta& meta, bool& exists) {
  exists = false;
  if (!Storage.exists(path_.c_str())) return ReadResult::Ok;  // no history yet
  if (!Storage.openFileForRead("RST", path_.c_str(), file)) return ReadResult::IoError;
  uint8_t live = kNoCopy;
  switch (ReadingStatsSlotFile::loadMeta(file, meta, live)) {
    case Load::Ok:
      exists = true;
      return ReadResult::Ok;
    case Load::Corrupt:
      return ReadResult::Corrupt;
    case Load::IoError:
      break;
  }
  return ReadResult::IoError;
}

bool ReadingStatsStore::createFresh(Meta& meta) {
  Storage.mkdir(parentDirOf(path_).c_str());
  const std::string tmpPath = path_ + ".tmp";
  meta.clear();
  meta.setSeq(1);
  meta.seal();
  bool written = ReadingStatsSlotFile::createZeroed(tmpPath.c_str());
  if (written) {
    FsFile file;
    written = Storage.openFileForUpdate("RST", tmpPath.c_str(), file) && ReadingStatsSlotFile::writeMeta(file, meta, 0);
    if (written) file.flush();
  }
  if (!written || !Storage.rename(tmpPath.c_str(), path_.c_str())) {
    LOG_ERR("RST", "Could not create %s", path_.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

ReadingStatsStore::ReadResult ReadingStatsStore::importLegacy() {
  [[maybe_unused]] const uint32_t started = millis();
  const std::string tmpPath = path_ + ".tmp";
  auto meta = Meta::create();
  auto image = makeUniqueNoThrow<uint8_t[]>(kSlotSize);
  if (!meta || !image) return ReadResult::NoMemory;
  Storage.mkdir(parentDirOf(path_).c_str());
  if (!ReadingStatsSlotFile::createZeroed(tmpPath.c_str())) return ReadResult::IoError;

  // One pass in file order: each book goes into the next slot as the parser finishes it, so an
  // import followed by an export gives the file back.
  ReadingStatsJson::ScanResult scanned = ReadingStatsJson::ScanResult::Ok;
  bool written = true;
  {
    FsFile in;
    FsFile out;
    if (!Storage.openFileForRead("RST", legacyPath_.c_str(), in) ||
        !Storage.openFileForUpdate("RST", tmpPath.c_str(), out)) {
      scanned = ReadingStatsJson::ScanResult::IoError;
    } else if (in.size() > 0) {  // a zero-byte file is an empty history, as it always was
      ReadingStatsJson::ScanRequest request;
      request.onBook = [&](const BookReadingStats& book) {
        DocKey key{};
        if (!written) return;
        if (!ReadingStatsSlotFile::parseDocId(book.docId, key)) {
          LOG_ERR("RST", "Import: '%s' is not a document id; skipped", book.docId.c_str());
          return;
        }
        if (meta->find(key) != kEntryCount) {
          LOG_ERR("RST", "Import: %s appears twice; the first kept", book.docId.c_str());
          return;
        }
        size_t index = meta->freeEntry();
        uint8_t slot = meta->freeSlot();
        if (index == kEntryCount) {
          // Past the cap (only a hand-made file gets here): keep what the cap would keep. The book
          // the cap evicts first loses its place, or this one never takes one.
          index = victimOf(*meta);
          const Entry worst = meta->entry(index);
          if (evictsBefore(book.lastReadEpoch, book.totalSeconds, static_cast<time_t>(worst.lastReadEpoch),
                           worst.totalSeconds)) {
            return;
          }
          slot = worst.slot;  // a new file: nothing else will ever refer to it
        }
        ReadingStatsSlotFile::encodeSlot(key, book, image.get());
        written = ReadingStatsSlotFile::writeSlot(out, slot, image.get());
        meta->setEntry(index, ReadingStatsSlotFile::entryFor(key, book, slot));
      };
      ReadingStatsJson::Summary legacy;
      scanned = ReadingStatsJson::scan(in, legacy, request);
      // The scan trimmed the days to the cap.
      if (scanned == ReadingStatsJson::ScanResult::Ok && !meta->writeTotals(legacy)) written = false;
    }
    if (scanned == ReadingStatsJson::ScanResult::Ok && written) {
      meta->setSeq(1);
      meta->seal();
      written = ReadingStatsSlotFile::writeMeta(out, *meta, 0);
      out.flush();
      uint8_t live = kNoCopy;
      const size_t books = meta->bookCount();
      written = written && ReadingStatsSlotFile::loadMeta(out, *meta, live) == Load::Ok && meta->bookCount() == books;
    }
  }
  if (scanned != ReadingStatsJson::ScanResult::Ok || !written) Storage.remove(tmpPath.c_str());
  switch (scanned) {
    case ReadingStatsJson::ScanResult::Ok:
      break;
    case ReadingStatsJson::ScanResult::NoMemory:
      return ReadResult::NoMemory;
    case ReadingStatsJson::ScanResult::IoError:
      return ReadResult::IoError;
    case ReadingStatsJson::ScanResult::Malformed:
      // As the JSON loader always did: set it aside, and the history starts afresh.
      return setAside(legacyPath_) ? ReadResult::Ok : ReadResult::IoError;
  }
  if (!written) {
    LOG_ERR("RST", "Import did not write or read back; %s left as is", legacyPath_.c_str());
    return ReadResult::IoError;
  }
  if (!Storage.rename(tmpPath.c_str(), path_.c_str())) {
    Storage.remove(tmpPath.c_str());
    return ReadResult::IoError;
  }
  const std::string done = legacyPath_ + ".imported";
  Storage.remove(done.c_str());
  if (!Storage.rename(legacyPath_.c_str(), done.c_str())) {
    LOG_ERR("RST", "Imported, but %s could not be renamed; it is ignored from now on", legacyPath_.c_str());
  }
  LOG_INF("RST", "Imported %u books from %s in %lu ms", static_cast<unsigned>(meta->bookCount()), legacyPath_.c_str(),
          static_cast<unsigned long>(millis() - started));
  return ReadResult::Ok;
}

// ---- Queries ----------------------------------------------------------------------------------

ReadingStatsStore::ReadResult ReadingStatsStore::querySummary(Summary& out, const bool withIndex) {
  out = Summary{};
  const ReadResult prepared = prepare();
  if (prepared != ReadResult::Ok) return prepared;
  auto meta = Meta::create();
  if (!meta) return ReadResult::NoMemory;
  FsFile file;
  bool exists = false;
  const ReadResult result = open(file, *meta, exists);
  if (result == ReadResult::Ok && exists) summarize(*meta, out, withIndex);
  return result;
}

ReadingStatsStore::ReadResult ReadingStatsStore::queryBook(const std::string& docId, BookQuery& out) {
  out = BookQuery{};
  DocKey key{};
  if (!ReadingStatsSlotFile::parseDocId(docId, key)) return ReadResult::Ok;  // no book of ours
  const ReadResult prepared = prepare();
  if (prepared != ReadResult::Ok) return prepared;
  auto meta = Meta::create();
  auto image = makeUniqueNoThrow<uint8_t[]>(kSlotSize);
  if (!meta || !image) return ReadResult::NoMemory;
  FsFile file;
  bool exists = false;
  const ReadResult result = open(file, *meta, exists);
  if (result != ReadResult::Ok || !exists) return result;
  out.pooledPace = pooledPaceOf(*meta);
  const size_t index = meta->find(key);
  if (index == kEntryCount) return ReadResult::Ok;
  readBook(file, meta->entry(index), image.get(), out.book);
  out.found = true;
  return ReadResult::Ok;
}

ReadingStatsStore::ReadResult ReadingStatsStore::queryBooksAt(const std::vector<IndexEntry>& index, const size_t first,
                                                              const size_t count, const uint32_t seq,
                                                              std::vector<BookReadingStats>& books) {
  books.clear();
  const size_t last = std::min(index.size(), first + count);
  // The rows outlive this call: on the heap before the meta's transient 6 KB (see prefetchRecent).
  books.reserve(last > first ? last - first : 0);
  const ReadResult prepared = prepare();
  if (prepared != ReadResult::Ok) return prepared;
  auto meta = Meta::create();
  auto image = makeUniqueNoThrow<uint8_t[]>(kSlotSize);
  if (!meta || !image) return ReadResult::NoMemory;
  FsFile file;
  bool exists = false;
  const ReadResult result = open(file, *meta, exists);
  if (result != ReadResult::Ok) return result;
  // A book's old slot keeps a valid copy of it after the book moves, so only the generation can
  // tell the index is old.
  if ((exists ? meta->seq() : 0) != seq) return ReadResult::Stale;
  for (size_t i = first; i < last; ++i) {
    BookReadingStats book;
    const size_t at = meta->find(index[i].key);
    if (at != kEntryCount) readBook(file, meta->entry(at), image.get(), book);
    books.push_back(std::move(book));
  }
  return ReadResult::Ok;
}

// ---- Home -------------------------------------------------------------------------------------

void ReadingStatsStore::prefetchRecent(const std::vector<std::string>& docIds) {
  // The snapshots outlive this call, so they go on the heap before the meta's transient 6 KB: a
  // long-lived block placed after a freed one splits the free space it leaves.
  std::vector<RecentSnapshot> snapshots(docIds.size());
  for (size_t i = 0; i < docIds.size(); ++i) snapshots[i].docId = docIds[i];
  float pace = 0.0f;
  ReadResult result = prepare();
  if (result == ReadResult::Ok) {
    auto meta = Meta::create();
    FsFile file;
    bool exists = false;
    result = meta ? open(file, *meta, exists) : ReadResult::NoMemory;
    if (result == ReadResult::Ok && exists) {
      for (RecentSnapshot& snapshot : snapshots) {
        DocKey key{};
        if (!ReadingStatsSlotFile::parseDocId(snapshot.docId, key)) continue;
        const size_t index = meta->find(key);
        if (index == kEntryCount) continue;
        const Entry entry = meta->entry(index);
        snapshot.known = true;
        snapshot.totalSeconds = entry.totalSeconds;
        snapshot.knownDays = entry.dayCount;
        snapshot.lastReadEpoch = static_cast<time_t>(entry.lastReadEpoch);
        snapshot.progress = entry.progress;
      }
      pace = pooledPaceOf(*meta);
    }
  }
  if (result != ReadResult::Ok) {
    LOG_ERR("RST", "prefetchRecent: history not read (%u); Home draws none this time", static_cast<unsigned>(result));
  }
  recent_ = std::move(snapshots);
  pooledPace_ = pace;
}

const ReadingStatsStore::RecentSnapshot* ReadingStatsStore::recent(const std::string& docId) const {
  for (const auto& snapshot : recent_) {
    if (snapshot.docId == docId) return &snapshot;
  }
  return nullptr;
}

// ---- The web ----------------------------------------------------------------------------------

ReadingStatsStore::ReadResult ReadingStatsStore::writeDashboard(Print& out, const uint16_t today,
                                                                const std::function<void()>& ready) {
  return writeJson(out, today, /*dashboard=*/true, ready);
}

ReadingStatsStore::ReadResult ReadingStatsStore::writeExport(Print& out, const std::function<void()>& ready) {
  return writeJson(out, 0, /*dashboard=*/false, ready);
}

ReadingStatsStore::ReadResult ReadingStatsStore::writeJson(Print& out, const uint16_t today, const bool dashboard,
                                                           const std::function<void()>& ready) {
  const ReadResult prepared = prepare();
  if (prepared != ReadResult::Ok) return prepared;
  auto meta = Meta::create();
  auto image = makeUniqueNoThrow<uint8_t[]>(kSlotSize);
  if (!meta || !image) return ReadResult::NoMemory;
  FsFile file;
  bool exists = false;
  const ReadResult result = open(file, *meta, exists);
  if (result != ReadResult::Ok) return result;
  Summary summary;
  if (exists) summarize(*meta, summary, /*withIndex=*/false);
  // Nothing below can fail: a damaged slot degrades to its directory figures.
  if (ready) ready();
  if (dashboard) {
    ReadingStatsJson::writeDashboardHead(out, summary, summary.bookCount, summary.finishedBookCount, today);
  } else {
    ReadingStatsJson::writeFileHead(out, summary);
  }
  const float pooled = pooledSecondsPerPercent(summary.totalSeconds, summary.paceSeconds, summary.pacePercents);
  bool first = true;
  for (size_t i = 0; exists && i < kEntryCount; ++i) {
    const Entry entry = meta->entry(i);
    if (!entry.used()) continue;
    BookReadingStats book;
    readBook(file, entry, image.get(), book);
    if (!first) ReadingStatsJson::writeBookSeparator(out);
    first = false;
    if (dashboard) {
      const float own = ownSecondsPerPercent(book.totalSeconds, book.progress);
      const float remaining = book.progress < 100 ? 100.0f - static_cast<float>(book.progress) : 0.0f;
      ReadingStatsJson::writeBook(out, book, etaSeconds(own > 0.0f ? own : pooled, remaining));
    } else {
      ReadingStatsJson::writeBook(out, book);
    }
  }
  ReadingStatsJson::writeTail(out);
  return ReadResult::Ok;
}

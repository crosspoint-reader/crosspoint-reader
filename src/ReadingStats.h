#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "ReadingStatsJson.h"
#include "ReadingStatsSlotFile.h"
#include "ReadingStatsTypes.h"

// Helpers — both return 0 when HalClock is unsynced (caller should skip).
uint16_t localDayIndexFromEpoch(time_t epoch);
uint16_t currentLocalDayIndex();

// The reading history: /.crosspoint/reading-stats.bin (ReadingStatsSlotFile), one file with a fixed
// place for every book. An update writes that book and the global figures, copy-on-write, never
// the whole history: rewriting the old single JSON file took 2.7 s at a worst-case session end on
// the X3. A reading-stats.json left by older firmware is imported into it once (prepare()).
//
// The store holds no history: each call reads what it needs (the ~6 KB meta, a slot or two) and
// lets it go. The only resident state is Home's snapshots (prefetchRecent). Call from the loop task,
// never from render().
//
// Deleting a book from the card never touches its history: only removeBook() takes a book out.
class ReadingStatsStore {
  static ReadingStatsStore instance;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  explicit ReadingStatsStore(std::string path = ReadingStatsSlotFile::kPath,
                             std::string legacyPath = ReadingStatsJson::kPath)
      : path_(std::move(path)), legacyPath_(std::move(legacyPath)) {}

  // ---- Updates ----------------------------------------------------------------------------------
  enum class WriteResult : uint8_t { Done, NotFound, NoMemory, Failed };

  // A finished session: creates the book's entry when the session has seconds (a book opened and
  // closed without reading is not history), evicting the least recently read book past kMaxBooks;
  // a zero-second session still moves an existing book's progress.
  WriteResult recordSession(const std::string& docId, const std::string& title, const std::string& author,
                            uint32_t sessionSeconds, uint32_t sessionPagesTurned, uint8_t progress,
                            time_t walltimeEpoch);

  // One more finish; creates the entry when needed, under the same cap.
  WriteResult markFinished(const std::string& docId, const std::string& title, const std::string& author,
                           time_t walltimeEpoch);

  // Forget one book: its entry goes and its time, sessions, pages and day buckets come back out of
  // the totals, so the stats read as if it had never been read. Exact for everything the screens
  // show — a book keeps its newest kMaxBookDays buckets, which always cover the 30-day sparkline
  // and the current streak. The persisted longest-streak record stays.
  WriteResult removeBook(const std::string& docId);
  // Carries a book's history over to a new document id: the id is a hash of the file NAME, so a
  // book renamed on the card would otherwise start a second, empty history. NotFound when `fromDocId`
  // has none; Failed when `toDocId` already has one (the two are left apart rather than merged).
  WriteResult renameBook(const std::string& fromDocId, const std::string& toDocId);

  // ---- Queries ----------------------------------------------------------------------------------
  //
  // Each reads the history and returns only what was asked for; nothing stays resident. No history
  // file is an empty history (Ok). Corrupt: the file is unreadable; the next update sets it aside
  // and starts a fresh history.
  enum class ReadResult : uint8_t { Ok, NoMemory, IoError, Corrupt, Stale };

  // A book in the time order.
  struct IndexEntry {
    uint32_t totalSeconds = 0;
    ReadingStatsSlotFile::DocKey key{};
  };

  struct Summary : ReadingTotals {
    uint32_t bookCount = 0;
    uint32_t finishedBookCount = 0;
    // Over the books far enough in to count toward the global pace.
    uint32_t paceSeconds = 0;
    uint32_t pacePercents = 0;
    // The history's generation when read (0: no history). Every update raises it; queryBooksAt()
    // compares it to tell that the index went stale.
    uint32_t seq = 0;
    // withIndex only: descending by time; equal times in directory order.
    std::vector<IndexEntry> byTime;
  };

  struct BookQuery {
    bool found = false;
    BookReadingStats book;
    float pooledPace = 0.0f;  // the global pace, for a book without one of its own
  };

  // What Home draws for one recent book.
  struct RecentSnapshot {
    std::string docId;
    bool known = false;  // false: the history holds no entry for this book
    uint32_t totalSeconds = 0;
    uint16_t knownDays = 0;  // dated reading days the book keeps (at most kMaxBookDays)
    time_t lastReadEpoch = 0;
    uint8_t progress = 0;
  };

  ReadResult querySummary(Summary& out, bool withIndex = false);
  ReadResult queryBook(const std::string& docId, BookQuery& out);
  // The books index[first, first + count) names, read with one load of the meta. Stale when the
  // history changed since the index was taken (`seq`, from its Summary): take the index again.
  // A book gone in between comes back empty.
  ReadResult queryBooksAt(const std::vector<IndexEntry>& index, size_t first, size_t count, uint32_t seq,
                          std::vector<BookReadingStats>& books);

  // ---- Home -------------------------------------------------------------------------------------
  //
  // Home calls prefetchRecent() on entry: one read of the meta gives a snapshot for each book asked
  // for (known or not) and the global pace, so the themes read no file from render(). They stay
  // until the next prefetch.
  void prefetchRecent(const std::vector<std::string>& docIds);
  const RecentSnapshot* recent(const std::string& docId) const;
  float recentPooledPace() const { return pooledPace_; }

  // ---- The web ----------------------------------------------------------------------------------
  //
  // Generated one book at a time: the meta and one slot, whatever the history holds.
  // `ready` runs once the history is open and every buffer is allocated, before the first byte:
  // the web handler sends its 200 there, so every failure comes before the response starts.
  // The /api/stats payload: the figures, streaks when `today` is known, every book with its
  // time-to-finish estimate (etaSeconds).
  ReadResult writeDashboard(Print& out, uint16_t today, const std::function<void()>& ready = nullptr);
  // The history in the reading-stats.json format, which older firmware reads.
  ReadResult writeExport(Print& out, const std::function<void()>& ready = nullptr);

  // ---- Reading speed / time-to-finish -----------------------------------------------------------
  //
  // Average seconds spent reading per 1% of book progress: a book's own once it has covered enough
  // ground to be meaningful, else the global average over the books that have. The minimum-progress
  // gate keeps 30 seconds at 2% from extrapolating to a 25-minute book.
  static constexpr uint8_t MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE = 3;  // %
  static constexpr uint32_t MIN_GLOBAL_SECONDS_FOR_RATE = 60;        // s

  // ---- The arithmetic, on plain data ------------------------------------------------------------
  //
  // Shared by the writes, the screens and the web, so the device screens and the web dashboard
  // agree on every figure.
  static uint32_t secondsOn(const std::vector<DayBucket>& days, uint16_t dayIndex);
  static uint16_t currentStreakIn(const std::vector<DayBucket>& days, uint16_t today);
  // The longest run in `days`, or the persisted `record` when that is longer.
  static uint16_t longestStreakIn(const std::vector<DayBucket>& days, uint16_t record);
  static bool countsTowardPace(const uint8_t progress) { return progress >= MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE; }
  // The cap's eviction order: the least recently read book goes first (an entry never read with
  // the clock set, lastReadEpoch 0, is the oldest); on the same date, the one with less time.
  static bool evictsBefore(time_t aLastRead, uint32_t aSeconds, time_t bLastRead, uint32_t bSeconds);
  // The global pace from the sums over the books that count toward it; 0 below
  // MIN_GLOBAL_SECONDS_FOR_RATE of reading overall.
  static float pooledSecondsPerPercent(uint32_t globalTotalSeconds, uint32_t countedSeconds, uint32_t countedPercents);
  // A book's own pace, or 0 when it has not covered enough ground to have one.
  static float ownSecondsPerPercent(uint32_t totalSeconds, uint8_t progress);
  static uint32_t etaSeconds(float secondsPerPercent, float remainingPercent);
  // Past the caps, the oldest buckets go; the global trim first folds the streak they held into
  // `record`, since the record may live in them.
  static void trimGlobalDays(std::vector<DayBucket>& days, uint16_t& record);
  static void trimBookDays(std::vector<DayBucket>& days);
  // Takes one book's contribution back out of the global figures (removal's arithmetic).
  static void takeOut(const BookReadingStats& book, ReadingTotals& totals);
  // A finished session applied to one book and the global figures (the session end's arithmetic):
  // counters, the book's and the global day buckets, the streak record. `title` / `author` replace
  // the book's only when non-empty.
  static void applySession(BookReadingStats& book, ReadingTotals& totals, const std::string& title,
                           const std::string& author, uint32_t sessionSeconds, uint32_t sessionPagesTurned,
                           uint8_t progress, time_t walltimeEpoch);
  // One more finish of the book.
  static void applyFinish(BookReadingStats& book, const std::string& title, const std::string& author,
                          time_t walltimeEpoch);

  // Bounds (memory audit 2026-09, R8). Past these caps the least recently read book goes, and the
  // oldest day buckets go; the sparkline needs 30 days and the streak walk needs the current run,
  // both well inside the global window, and the longest streak is kept as a number. The file's
  // layout holds exactly these (static_asserts below).
  static constexpr size_t kMaxBooks = 100;
  static constexpr size_t kMaxBookDays = 60;
  static constexpr size_t kMaxGlobalDays = 400;

 private:
  enum class Edit : uint8_t { Session, Finish, Remove, Rekey };
  // The update shared by the writes. `apply` changes the book and the totals and returns false when
  // there is nothing to write. A Rekey stores the book under `newDocId` in place of `docId`.
  WriteResult write(Edit edit, const std::string& docId,
                    const std::function<bool(BookReadingStats& book, ReadingTotals& totals, bool existed)>& apply,
                    const std::string& newDocId = "");
  // Before anything reads the history: a create or an import cut short is discarded, and the
  // legacy JSON imported once. Ok from its first success on.
  ReadResult prepare();
  // The history file open for reading with its meta loaded; `exists` false when there is none.
  ReadResult open(FsFile& file, ReadingStatsSlotFile::Meta& meta, bool& exists);
  // A new, empty history file, written as a temporary file and renamed into place, so a card never
  // holds half of one. Leaves `meta` as written: empty, seq 1, in copy A.
  bool createFresh(ReadingStatsSlotFile::Meta& meta);
  // The reading-stats.json of older firmware, into a new history file: once, on the first call
  // that finds no history file beside it. The JSON is only read, then renamed *.imported.
  ReadResult importLegacy();
  // Both web payloads: the head from the meta, then every book from its slot.
  ReadResult writeJson(Print& out, uint16_t today, bool dashboard, const std::function<void()>& ready);

  std::string path_;
  std::string legacyPath_;
  bool prepared_ = false;
  std::vector<RecentSnapshot> recent_;
  float pooledPace_ = 0.0f;
};

static_assert(ReadingStatsStore::kMaxBooks == ReadingStatsSlotFile::kEntryCount, "the directory holds the cap");
static_assert(ReadingStatsStore::kMaxBookDays == ReadingStatsSlotFile::kBookDayCapacity, "a slot holds the day cap");
static_assert(ReadingStatsStore::kMaxGlobalDays == ReadingStatsSlotFile::kGlobalDayCapacity,
              "the meta holds the global day cap");

#define READING_STATS ReadingStatsStore::getInstance()

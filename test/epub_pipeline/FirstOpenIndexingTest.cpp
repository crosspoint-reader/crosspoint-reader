// The cost of a cold first open, phase by phase, for a book with thousands of spine items.
//
// X3, 2026-10-20: the first open of King's Avatar (1732 spine items, ~1600 images) sat 431 s
// under "Indexing", 427 s of it in buildBookBin looking up each spine item's size with a walk of
// the ZIP central directory of its own. No phase of the open may cost walks in proportion to the
// spine, the TOC or the manifest: a walk reads every entry it passes, at about a dozen card calls
// an entry. This loads the same book shape at two sizes, as ReaderActivity's first open does (the
// secondary framebuffer lent, the heap the X3 logged), and holds each phase's walks, entries read
// and card calls to what one book four times the size of the other can justify.
//
// Measured on the 1732-item book (3336 entries) before the fix:
//   book.bin   1732 walks, 1923853 entries read, 25013124 card calls
//   css+rest      3 walks,    3346 entries read,    46978 card calls (the stylesheet discovery walk)
//   whole open 1744 walks, 1930565 entries read, 25061723 card calls
// and after it: book.bin 1 walk and 228 card calls, the whole open 13 walks and 2180 card calls.
#include <BuildArena.h>
#include <HalStorage.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "BigSpineBook.h"
#include "Epub.h"
#include "LogObserver.h"

namespace fs = std::filesystem;

namespace {

// The X3's secondary framebuffer, lent to first-open indexing (ReaderActivity::onEnter).
constexpr size_t kX3LentFramebuffer = 52272;
// The free heap the X3 logged at buildBookBin on 2026-10-20; its largest block was not logged.
constexpr size_t kX3FreeHeap = 43576;
constexpr size_t kX3LargestBlock = 24 * 1024;
// The X3 refused the manifest index its 24 KB growth (2026-10-20: "manifest index OOM at 1024
// items"), so its largest block during the OPF parse was smaller still.
constexpr size_t kX3OpfBlock = 20 * 1024;

struct Counters {
  size_t walks = 0;
  size_t entries = 0;
  size_t calls = 0;
  size_t opens = 0;
  size_t bytesRead = 0;
  size_t flushes = 0;
};

Counters now() {
  return {zipScanStats().walks, zipScanStats().entries, FsFile::calls,
          FsFile::opens,        FsFile::bytesRead,      FsFile::flushes};
}

Counters operator-(const Counters& a, const Counters& b) {
  return {a.walks - b.walks, a.entries - b.entries,     a.calls - b.calls,
          a.opens - b.opens, a.bytesRead - b.bytesRead, a.flushes - b.flushes};
}

// The phases of Epub::load's build, each ended by a log line containing the text named here.
enum Phase { Opf, Toc, BookBin, Css, Rest, PhaseCount };
const char* const kPhaseNames[PhaseCount] = {"opf", "toc", "book.bin", "css", "rest"};
const char* const kPhaseEnds[PhaseCount] = {"OPF pass completed", "TOC pass completed", "buildBookBin completed",
                                            "CSS style rules", "Loaded ePub"};

Counters g_atPhaseEnd[PhaseCount];
size_t g_heapPeak[PhaseCount];
size_t g_liveAtStart = 0;
int g_phase = 0;

void onLog(const char*, const char*, const char* message) {
  if (g_phase < PhaseCount && std::strstr(message, kPhaseEnds[g_phase]) != nullptr) {
    g_heapPeak[g_phase] = heapPeakBytes() - g_liveAtStart;
    resetHeapPeak();
    g_atPhaseEnd[g_phase++] = now();
  }
}

struct Open {
  bool loaded = false;
  size_t archiveEntries = 0;
  Counters phase[PhaseCount];
  size_t heapPeak[PhaseCount] = {};
  Counters total;
};

// Every block larger than this refused (0: none), as a fragmented heap would.
Open firstOpen(const fs::path& work, const int chapters, const big_spine_book::Options& options = {},
               const size_t largestBlock = kX3LargestBlock, const size_t refuseAbove = 0) {
  const big_spine_book::Book book = big_spine_book::write(
      (work / ("book" + std::to_string(chapters) + ".epub")).string(), chapters, chapters * 12 / 13, options);
  auto lent = std::make_unique<uint8_t[]>(kX3LentFramebuffer);
  BuildArena scratch(lent.get(), kX3LentFramebuffer);
  fs::remove_all(work / "cache");

  Open open;
  open.archiveEntries = book.entries;
  zipScanStats() = {};
  FsFile::calls = 0;
  FsFile::opens = 0;
  FsFile::bytesRead = 0;
  FsFile::flushes = 0;
  g_phase = 0;
  g_liveAtStart = heapLiveBytes();
  resetHeapPeak();
  setLogObserver(onLog);
  {
    const HostFreeHeap freeHeap(kX3FreeHeap);
    const HostLargestFreeBlock largest(largestBlock);
    std::optional<AllocationRefusal> refusal;
    if (refuseAbove != 0) refusal.emplace(refuseAbove + 1);
    Epub epub(book.path, (work / "cache").string());
    open.loaded = epub.load(true, false, &scratch) && epub.getSpineItemsCount() == chapters;
  }
  setLogObserver(nullptr);
  EXPECT_EQ(g_phase, PhaseCount) << "a phase-ending log line was not seen";
  Counters before;
  for (int p = 0; p < PhaseCount; ++p) {
    open.phase[p] = g_atPhaseEnd[p] - before;
    open.heapPeak[p] = g_heapPeak[p];
    before = g_atPhaseEnd[p];
  }
  open.total = before;
  EXPECT_EQ(scratch.used(), 0u) << "the lent region must come back empty";
  return open;
}

// On the X3 a card call costs about 17 us (427 s for 25.0 M of them).
double x3Seconds(const Counters& c) { return static_cast<double>(c.calls) * 17e-6; }

void report(const char* label, const int chapters, const Open& open) {
  for (int p = 0; p < PhaseCount; ++p) {
    const Counters& c = open.phase[p];
    std::printf(
        "[ firstopen] %-8s spines=%-5d %-9s walks=%-5zu entriesRead=%-8zu fileCalls=%-9zu (~%.2f s on the X3) "
        "bytesRead=%-9zu opens=%-3zu flushes=%-4zu heapPeak=%zu\n",
        label, chapters, kPhaseNames[p], c.walks, c.entries, c.calls, x3Seconds(c), c.bytesRead, c.opens, c.flushes,
        open.heapPeak[p]);
  }
  std::printf(
      "[ firstopen] %-8s spines=%-5d %-9s walks=%-5zu entriesRead=%-8zu fileCalls=%-9zu (~%.2f s on the X3) "
      "bytesRead=%-9zu opens=%-3zu (archive: %zu)\n",
      label, chapters, "total", open.total.walks, open.total.entries, open.total.calls, x3Seconds(open.total),
      open.total.bytesRead, open.total.opens, open.archiveEntries);
}

TEST(FirstOpenIndexing, NoPhaseWalksTheCentralDirectoryInProportionToTheSpine) {
  const fs::path work = fs::temp_directory_path() / ("first_open_indexing_" + std::to_string(getpid()));
  fs::remove_all(work);
  fs::create_directories(work);

  const Open small = firstOpen(work, 433);
  const Open large = firstOpen(work, 1732);
  ASSERT_TRUE(small.loaded);
  ASSERT_TRUE(large.loaded);
  report("default", 433, small);
  report("default", 1732, large);

  for (int p = 0; p < PhaseCount; ++p) {
    // Four times the book may read four times the entries and make four times the card calls,
    // and walk no more often.
    EXPECT_LE(large.phase[p].walks, small.phase[p].walks + 1) << kPhaseNames[p];
    EXPECT_LE(large.phase[p].entries, 5 * small.phase[p].entries + 64) << kPhaseNames[p];
    EXPECT_LE(large.phase[p].calls, 5 * small.phase[p].calls + 256) << kPhaseNames[p];
  }
  // A handful of walks in all: one for each document the open reads by name, not one per spine item.
  EXPECT_LE(large.total.walks, 32u);
  EXPECT_EQ(large.phase[BookBin].walks, 1u) << "the X3's lent framebuffer holds the whole size table";
  // Every walk reads through a window: under two card calls per archive entry for the whole open,
  // where one walk field by field was ~14 an entry.
  EXPECT_LT(large.total.calls, 2 * large.archiveEntries);
  fs::remove_all(work);
}

// King's Avatar as the X3's numbers describe it (2026-10-20, with the size table fixed: OPF 2.2 s,
// TOC 2.3 s, two stylesheets 1.7 s, "manifest index OOM at 1024 items"): the package documents sit
// behind every chapter and image, the manifest is in name order, two stylesheets of 400 rules, and
// no heap block over 20 KB is granted. Card calls, then, by phase (before -> after the lookups by
// name, the OPF's spine passes and the CSS staging writes): see the report lines.
TEST(FirstOpenIndexing, AKingsAvatarProfileReadsEachDocumentOnceAndIndexesNothingInRam) {
  const fs::path work = fs::temp_directory_path() / ("first_open_profile_" + std::to_string(getpid()));
  fs::remove_all(work);
  fs::create_directories(work);
  big_spine_book::Options options;
  options.manifestInNameOrder = true;
  options.documentsLast = true;
  options.stylesheets = 2;
  options.rulesPerStylesheet = 400;
  const Open small = firstOpen(work, 433, options, kX3OpfBlock, kX3OpfBlock);
  const Open large = firstOpen(work, 1732, options, kX3OpfBlock, kX3OpfBlock);
  ASSERT_TRUE(small.loaded);
  ASSERT_TRUE(large.loaded);
  report("profile", 433, small);
  report("profile", 1732, large);

  for (int p = 0; p < PhaseCount; ++p) {
    EXPECT_LE(large.phase[p].walks, small.phase[p].walks + 1) << kPhaseNames[p];
    EXPECT_LE(large.phase[p].calls, 5 * small.phase[p].calls + 256) << kPhaseNames[p];
  }
  // A document's lookups through a window: the OPF and TOC phases make fewer card calls than the
  // archive has entries (one field-by-field walk was ~14 an entry: ~87000 each before).
  EXPECT_LT(large.phase[Opf].calls, 2 * large.archiveEntries);
  EXPECT_LT(large.phase[Toc].calls, large.archiveEntries);
  // 800 rules staged a call per rule record field and a sync each: ~178000 calls before.
  EXPECT_LT(large.phase[Css].calls, 16000u);
  EXPECT_EQ(large.phase[Css].flushes, 0u) << "no rule here is merged, so nothing needs syncing";
  // No manifest index: the OPF parse holds the parser, its buffers and the spine's table only.
  EXPECT_LT(large.heapPeak[Opf], 32u * 1024);
  fs::remove_all(work);
}

// The build tells its caller how far it has got, at each phase's end, for the popup's progress bar;
// a load from the cache does not.
std::vector<int> g_progress;

TEST(FirstOpenIndexing, ABuildReportsItsProgressAtEachPhaseEnd) {
  const fs::path work = fs::temp_directory_path() / ("first_open_progress_" + std::to_string(getpid()));
  fs::remove_all(work);
  fs::create_directories(work);
  const big_spine_book::Book book = big_spine_book::write((work / "book.epub").string(), 60, 50);
  const auto onProgress = [](void*, const uint8_t percent) { g_progress.push_back(percent); };

  g_progress.clear();
  {
    Epub epub(book.path, (work / "cache").string());
    epub.setBuildProgress(onProgress, nullptr);
    ASSERT_TRUE(epub.load(true, false));
  }
  EXPECT_EQ(g_progress, (std::vector<int>{35, 65, 85}));

  g_progress.clear();
  {
    Epub epub(book.path, (work / "cache").string());
    epub.setBuildProgress(onProgress, nullptr);
    ASSERT_TRUE(epub.load(true, false));
  }
  EXPECT_TRUE(g_progress.empty()) << "a warm load builds nothing";
  fs::remove_all(work);
}

}  // namespace

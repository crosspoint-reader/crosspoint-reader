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
#include <string>
#include <vector>

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

struct Counters {
  size_t walks = 0;
  size_t entries = 0;
  size_t calls = 0;
  size_t opens = 0;
};

Counters now() { return {zipScanStats().walks, zipScanStats().entries, FsFile::calls, FsFile::opens}; }

Counters operator-(const Counters& a, const Counters& b) {
  return {a.walks - b.walks, a.entries - b.entries, a.calls - b.calls, a.opens - b.opens};
}

// The phases of Epub::load's build, each ended by the log line named here.
enum Phase { Opf, Toc, BookBin, Rest, PhaseCount };
const char* const kPhaseNames[PhaseCount] = {"opf", "toc", "book.bin", "css+rest"};
const char* const kPhaseEnds[PhaseCount] = {"OPF pass completed", "TOC pass completed", "buildBookBin completed",
                                            "Loaded ePub"};

Counters g_atPhaseEnd[PhaseCount];
int g_phase = 0;

void onLog(const char*, const char*, const char* message) {
  if (g_phase < PhaseCount && std::strncmp(message, kPhaseEnds[g_phase], std::strlen(kPhaseEnds[g_phase])) == 0) {
    g_atPhaseEnd[g_phase++] = now();
  }
}

struct Open {
  bool loaded = false;
  size_t archiveEntries = 0;
  Counters phase[PhaseCount];
  Counters total;
};

Open firstOpen(const fs::path& work, const int chapters) {
  const big_spine_book::Book book = big_spine_book::write(
      (work / ("book" + std::to_string(chapters) + ".epub")).string(), chapters, chapters * 12 / 13);
  auto lent = std::make_unique<uint8_t[]>(kX3LentFramebuffer);
  BuildArena scratch(lent.get(), kX3LentFramebuffer);

  Open open;
  open.archiveEntries = book.entries;
  zipScanStats() = {};
  FsFile::calls = 0;
  FsFile::opens = 0;
  g_phase = 0;
  setLogObserver(onLog);
  {
    const HostFreeHeap freeHeap(kX3FreeHeap);
    const HostLargestFreeBlock largest(kX3LargestBlock);
    Epub epub(book.path, (work / "cache").string());
    open.loaded = epub.load(true, false, &scratch) && epub.getSpineItemsCount() == chapters;
  }
  setLogObserver(nullptr);
  EXPECT_EQ(g_phase, PhaseCount) << "a phase-ending log line was not seen";
  Counters before;
  for (int p = 0; p < PhaseCount; ++p) {
    open.phase[p] = g_atPhaseEnd[p] - before;
    before = g_atPhaseEnd[p];
  }
  open.total = before;
  EXPECT_EQ(scratch.used(), 0u) << "the lent region must come back empty";
  return open;
}

void report(const int chapters, const Open& open) {
  for (int p = 0; p < PhaseCount; ++p) {
    const Counters& c = open.phase[p];
    std::printf("[ firstopen] spines=%-5d %-9s walks=%-5zu entriesRead=%-8zu fileCalls=%-9zu opens=%zu\n", chapters,
                kPhaseNames[p], c.walks, c.entries, c.calls, c.opens);
  }
  std::printf("[ firstopen] spines=%-5d %-9s walks=%-5zu entriesRead=%-8zu fileCalls=%-9zu opens=%zu (archive: %zu)\n",
              chapters, "total", open.total.walks, open.total.entries, open.total.calls, open.total.opens,
              open.archiveEntries);
}

TEST(FirstOpenIndexing, NoPhaseWalksTheCentralDirectoryInProportionToTheSpine) {
  const fs::path work = fs::temp_directory_path() / ("first_open_indexing_" + std::to_string(getpid()));
  fs::remove_all(work);
  fs::create_directories(work);

  const Open small = firstOpen(work, 433);
  const Open large = firstOpen(work, 1732);
  ASSERT_TRUE(small.loaded);
  ASSERT_TRUE(large.loaded);
  report(433, small);
  report(1732, large);

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

}  // namespace

// The spine item sizes in book.bin, for a book with thousands of spine items, at the heaps that
// first-open indexing meets.
//
// X3, 2026-10-20: a cold open of King's Avatar (1732 spine items) sat 431 s under "Indexing".
// buildBookBin found 43.5 KB free, short of the 67 KB its batch tables asked for, and looked up
// every spine item's size with a walk of the ZIP central directory of its own: 1732 walks at
// ~246 ms each. These tests build book.bin for a book of that shape at several heap levels, with
// and without the lent framebuffer, and count the walks: a walk reads every entry it passes, and
// the card calls it makes are what the open costs on the device.
//
// Measured before the fix, on the same book (1732 spine items, 3338 entries):
//   full heap      1 walk,      3338 entries read,    46900 card calls, book.bin fnv 22dc626632b933f4
//   43.5 KB free   1732 walks,  1925005 entries read, 25028100 card calls (the X3's 427 s)
//
// The heap is simulated the way the device refuses it: the stubs report the free heap and the
// largest block, and AllocationRefusal fails every allocation larger than that block.

#include <BuildArena.h>
#include <HalStorage.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "AllocationRefusal.h"
#include "BigSpineBook.h"
#include "BookMetadataCache.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

struct Heap {
  const char* name;
  size_t freeBytes;
  size_t largestBlock;
};
// What the host stubs report by default.
constexpr Heap kPlenty{"plenty", 300 * 1024, 200 * 1024};
// The X3 open of 2026-10-20 logged 43576 B free at the size lookup; its largest block was not
// logged, and a fragmented heap of that size rarely has more than half of it in one piece.
constexpr Heap kX3Measured{"x3-measured", 43576, 24 * 1024};
constexpr Heap kVeryLow{"very-low", 12 * 1024, 6 * 1024};

// The X3's secondary framebuffer, which ReaderActivity lends to first-open indexing.
constexpr size_t kX3LentFramebuffer = 52272;

struct Cost {
  size_t walks = 0;
  size_t entries = 0;
  size_t calls = 0;
  size_t opens = 0;
  // Heap held at once above what was live when buildBookBin started (operator new, 64-bit host).
  size_t heapPeak = 0;
  size_t arenaPeak = 0;
  double ms = 0;
};

struct BuildResult {
  bool built = false;
  std::vector<char> bookBin;
  Cost cost;
  bool arenaReturnedEmpty = true;
};

std::string workDir() {
  const auto dir = fs::temp_directory_path() / ("bookbin_size_" + std::to_string(getpid()));
  fs::create_directories(dir);
  return dir.string();
}

std::vector<char> readAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// The spine and TOC passes as Epub::load drives them (a TOC entry per spine item, as the NCX of
// a web novel has), then buildBookBin under `heap`, lent `arenaBytes` of region when non-zero.
// Only buildBookBin is measured and runs on the simulated heap.
BuildResult buildBookBin(const std::string& zipPath, const std::vector<std::string>& spineHrefs,
                         const std::string& cacheDir, const Heap& heap, const size_t arenaBytes = 0) {
  fs::remove_all(cacheDir);
  fs::create_directories(cacheDir);
  BookMetadataCache cache(cacheDir);
  EXPECT_TRUE(cache.beginWrite());
  EXPECT_TRUE(cache.beginContentOpfPass());
  for (const std::string& href : spineHrefs) cache.createSpineEntry(href);
  EXPECT_TRUE(cache.endContentOpfPass());
  EXPECT_TRUE(cache.beginTocPass());
  for (size_t i = 0; i < spineHrefs.size(); ++i) {
    cache.createTocEntry("Chapter " + std::to_string(i), spineHrefs[i], "", 1);
  }
  EXPECT_TRUE(cache.endTocPass());
  EXPECT_TRUE(cache.endWrite());

  BookMetadataCache::BookMetadata metadata;
  metadata.title = "Big Spine";
  metadata.author = "Test";
  metadata.language = "en";

  // Allocated before the measurement, as the framebuffer is before the open.
  std::unique_ptr<uint8_t[]> region(arenaBytes > 0 ? new uint8_t[arenaBytes] : nullptr);
  BuildArena arena(region.get(), arenaBytes);

  BuildResult run;
  zipScanStats() = {};
  FsFile::calls = 0;
  FsFile::opens = 0;
  const size_t liveBefore = heapLiveBytes();
  resetHeapPeak();
  const auto start = std::chrono::steady_clock::now();
  {
    const HostFreeHeap freeHeap(heap.freeBytes);
    const HostLargestFreeBlock largest(heap.largestBlock);
    const AllocationRefusal refusal(heap.largestBlock + 1);
    run.built = cache.buildBookBin(zipPath, metadata, arena.valid() ? &arena : nullptr);
  }
  run.cost.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  run.cost.walks = zipScanStats().walks;
  run.cost.entries = zipScanStats().entries;
  run.cost.calls = FsFile::calls;
  run.cost.opens = FsFile::opens;
  run.cost.heapPeak = heapPeakBytes() - liveBefore;
  run.cost.arenaPeak = arena.highWater();
  run.arenaReturnedEmpty = arena.used() == 0;
  run.bookBin = readAll(cacheDir + "/book.bin");
  return run;
}

// FNV-1a 64 of book.bin, printed so a run on another build of the code can be compared byte for byte.
uint64_t digest(const std::vector<char>& bytes) {
  uint64_t h = 14695981039346656037ull;
  for (const char c : bytes) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
  return h;
}

void report(const char* label, const size_t spines, const size_t entries, const BuildResult& run) {
  const Cost& c = run.cost;
  std::printf(
      "[ bookbin  ] %-20s spines=%zu entries=%zu: walks=%zu entriesRead=%zu fileCalls=%zu opens=%zu heapPeak=%zu B "
      "arenaPeak=%zu B host=%.1f ms book.bin=%zu B fnv=%016llx\n",
      label, spines, entries, c.walks, c.entries, c.calls, c.opens, c.heapPeak, c.arenaPeak, c.ms, run.bookBin.size(),
      static_cast<unsigned long long>(digest(run.bookBin)));
}

// The cumulative sizes book.bin records, read back the way the reader reads them.
std::vector<uint32_t> cumulativeSizes(const std::string& cacheDir) {
  BookMetadataCache cache(cacheDir);
  std::vector<uint32_t> out;
  if (!cache.load()) {
    ADD_FAILURE() << "book.bin does not load";
    return out;
  }
  for (int i = 0; i < cache.getSpineCount(); ++i) out.push_back(cache.getSpineEntry(i).cumulativeSize);
  return out;
}

std::vector<uint32_t> expectedCumulative(const std::vector<uint32_t>& sizes) {
  std::vector<uint32_t> out;
  uint32_t sum = 0;
  for (const uint32_t s : sizes) out.push_back(sum += s);
  return out;
}

size_t walksFor(const size_t spines, const size_t perWalk) { return (spines + perWalk - 1) / perWalk; }

class BookBinSizeLookupTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = workDir();
    book_ = big_spine_book::write(dir_ + "/book.epub", 1732, 1600);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  std::string cacheDir() const { return dir_ + "/cache"; }

  std::string dir_;
  big_spine_book::Book book_;
};

TEST_F(BookBinSizeLookupTest, AKingsAvatarSizedBookTakesAHandfulOfWalksAtEveryHeapLevel) {
  const size_t spines = book_.spineHrefs.size();
  const BuildResult reference = buildBookBin(book_.path, book_.spineHrefs, cacheDir(), kPlenty);
  ASSERT_TRUE(reference.built);
  report("plenty", spines, book_.entries, reference);
  EXPECT_EQ(cumulativeSizes(cacheDir()), expectedCumulative(book_.spineSizes));
  EXPECT_EQ(reference.cost.walks, 1u);

  struct Case {
    const char* label;
    Heap heap;
    size_t arena;
    size_t maxWalks;
  };
  const Case cases[] = {
      {"plenty+lent", kPlenty, kX3LentFramebuffer, 1},
      // The X3's open: the table in the lent framebuffer, one walk.
      {"x3-measured+lent", kX3Measured, kX3LentFramebuffer, 1},
      // No region lent (a load outside ReaderActivity's first open): the largest block's worth.
      {"x3-measured", kX3Measured, 0, walksFor(spines, (kX3Measured.largestBlock - 64) / sizeof(ZipFile::SizeTarget))},
      {"very-low+lent", kVeryLow, kX3LentFramebuffer, 1},
      // 12 KB free: the 2 KB minimum table, 128 spine items a walk.
      {"very-low", kVeryLow, 0, walksFor(spines, 128)},
  };
  for (const Case& c : cases) {
    const BuildResult run = buildBookBin(book_.path, book_.spineHrefs, cacheDir(), c.heap, c.arena);
    ASSERT_TRUE(run.built) << c.label;
    report(c.label, spines, book_.entries, run);
    EXPECT_EQ(run.bookBin, reference.bookBin) << c.label << ": book.bin differs from the one built on a full heap";
    EXPECT_LE(run.cost.walks, c.maxWalks) << c.label << " (1732 walks before the fix)";
    EXPECT_LE(run.cost.entries, c.maxWalks * book_.entries) << c.label;
    EXPECT_TRUE(run.arenaReturnedEmpty) << c.label << ": the lent region must come back as it was lent";
  }
}

// Lent the framebuffer, the table and its window live there and the heap holds no more than the
// build's file buffers: no block of the table's size is asked of it at all.
TEST_F(BookBinSizeLookupTest, TheTableLivesInTheLentFramebufferNotTheHeap) {
  const BuildResult heapOnly = buildBookBin(book_.path, book_.spineHrefs, cacheDir(), kPlenty);
  const BuildResult lent = buildBookBin(book_.path, book_.spineHrefs, cacheDir(), kPlenty, kX3LentFramebuffer);
  ASSERT_TRUE(heapOnly.built);
  ASSERT_TRUE(lent.built);
  const size_t table = book_.spineHrefs.size() * sizeof(ZipFile::SizeTarget);
  EXPECT_GE(lent.cost.arenaPeak, table + 4096);
  EXPECT_LE(lent.cost.arenaPeak, kX3LentFramebuffer);
  EXPECT_GE(heapOnly.cost.heapPeak, lent.cost.heapPeak + table) << "the heap-only build holds the table on the heap";
  EXPECT_EQ(lent.bookBin, heapOnly.bookBin);

  // And with every block over 4 KB refused, the lent build still makes a single walk.
  const BuildResult fragmented =
      buildBookBin(book_.path, book_.spineHrefs, cacheDir(), Heap{"4k-blocks", 300 * 1024, 4096}, kX3LentFramebuffer);
  ASSERT_TRUE(fragmented.built);
  EXPECT_EQ(fragmented.cost.walks, 1u);
  EXPECT_EQ(fragmented.bookBin, heapOnly.bookBin);
}

// A spine larger than the lent region holds: a table-full per walk, the last one short.
TEST_F(BookBinSizeLookupTest, ASpineLargerThanTheLentRegionTakesATableFullPerWalk) {
  const BuildResult reference = buildBookBin(book_.path, book_.spineHrefs, cacheDir(), kPlenty);
  // Room for the window and 500 entries: 1732 spine items in four walks.
  const size_t region = 4096 + 2 * alignof(std::max_align_t) + 500 * sizeof(ZipFile::SizeTarget);
  const BuildResult run = buildBookBin(book_.path, book_.spineHrefs, cacheDir(), kPlenty, region);
  ASSERT_TRUE(run.built);
  EXPECT_EQ(run.cost.walks, 4u);
  EXPECT_EQ(run.bookBin, reference.bookBin);
  EXPECT_EQ(cumulativeSizes(cacheDir()), expectedCumulative(book_.spineSizes));
}

// Names the walk cannot match byte for byte -- an entry stored in another case, a spine item
// listed twice, one missing from the archive -- come out as the per-item lookup has them, and a
// missing one adds nothing to the cumulative size.
TEST_F(BookBinSizeLookupTest, NamesTheWalkCannotMatchAreLookedUpOnTheirOwn) {
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  std::vector<std::string> spine;
  std::vector<uint32_t> sizes;
  for (int i = 0; i < 40; ++i) {
    const std::string data(static_cast<size_t>(100 + i), 'x');
    // Chapter 7's entry is stored capitalised; the OPF names it in lower case.
    zip.add(i == 7 ? "OEBPS/Chapter7.xhtml" : "OEBPS/chapter" + std::to_string(i) + ".xhtml", data);
    spine.push_back("OEBPS/chapter" + std::to_string(i) + ".xhtml");
    sizes.push_back(static_cast<uint32_t>(data.size()));
  }
  spine.push_back("OEBPS/chapter3.xhtml");  // listed twice
  sizes.push_back(sizes[3]);
  spine.push_back("OEBPS/missing.xhtml");  // not in the archive
  sizes.push_back(0);
  const std::string path = dir_ + "/odd.epub";
  zip.write(path);

  const BuildResult lent = buildBookBin(path, spine, cacheDir(), kPlenty, kX3LentFramebuffer);
  ASSERT_TRUE(lent.built);
  EXPECT_EQ(cumulativeSizes(cacheDir()), expectedCumulative(sizes));
  // One walk for the batch, one lookup each for the capitalised and the missing entry.
  EXPECT_EQ(lent.cost.walks, 3u);
  const BuildResult heapOnly = buildBookBin(path, spine, cacheDir(), kVeryLow);
  ASSERT_TRUE(heapOnly.built);
  EXPECT_EQ(heapOnly.bookBin, lent.bookBin);
}

}  // namespace

// What FileBrowserModel holds on the heap for a folder of many entries. The device is built with
// -fno-exceptions, so a std::vector or std::string that cannot grow aborts and reboots it: a folder
// must cost the browser a bounded amount however many books it holds.
//
// Runs the real model over a real directory tree through the stdio storage shim, with the SD index
// replaced by a fake (FileIndex.h here) that keeps nothing per entry. Every operator new in the
// process is counted, so a test can ask what the model kept and what it needed at its peak. The
// shim lists a whole directory into RAM when it is opened, which the model's own peak is measured
// against: the same listing read by a plain loop.
#include <Arduino.h>
#include <BookDetails.h>
#include <CooperativeAbort.h>
#include <FileBrowserModel.h>
#include <LibraryFreshness.h>
#include <LibraryOrder.h>
#include <RecentBooksStore.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>

namespace fs = std::filesystem;

// ---- Heap accounting --------------------------------------------------------------------------

namespace heap {
long long live = 0;
long long peak = 0;

void* take(const size_t bytes) {
  auto* block = static_cast<std::max_align_t*>(std::malloc(bytes + sizeof(std::max_align_t)));
  if (block == nullptr) return nullptr;
  *reinterpret_cast<size_t*>(block) = bytes;
  live += static_cast<long long>(bytes);
  if (live > peak) peak = live;
  return block + 1;
}

void give(void* p) {
  if (p == nullptr) return;
  auto* block = static_cast<std::max_align_t*>(p) - 1;
  live -= static_cast<long long>(*reinterpret_cast<size_t*>(block));
  std::free(block);
}

// What was allocated and still held, and the high-water mark, since it was opened.
class Window {
 public:
  Window() : start(live) { peak = live; }
  [[nodiscard]] long long retained() const { return live - start; }
  [[nodiscard]] long long peakAbove() const { return peak - start; }

 private:
  long long start;
};
}  // namespace heap

void* operator new(const size_t bytes) {
  if (void* p = heap::take(bytes)) return p;
  throw std::bad_alloc();
}
void* operator new[](const size_t bytes) {
  if (void* p = heap::take(bytes)) return p;
  throw std::bad_alloc();
}
void* operator new(const size_t bytes, const std::nothrow_t&) noexcept { return heap::take(bytes); }
void* operator new[](const size_t bytes, const std::nothrow_t&) noexcept { return heap::take(bytes); }
void operator delete(void* p) noexcept { heap::give(p); }
void operator delete[](void* p) noexcept { heap::give(p); }
void operator delete(void* p, size_t) noexcept { heap::give(p); }
void operator delete[](void* p, size_t) noexcept { heap::give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { heap::give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { heap::give(p); }

// ---- What the model reaches beyond this test ----------------------------------------------------

CrossPointSettings CrossPointSettings::instance;
RecentBooksStore RecentBooksStore::instance;
RecentBooksStore::Hold::Hold() = default;
RecentBooksStore::Hold::~Hold() = default;
void RecentBooksStore::ensureLoaded() const {}
bool RecentBooksStore::pruneMissing() { return false; }
bool RecentBooksStore::saveToFile() const { return true; }

namespace LibraryFreshness {
void checkListedEntry(uint32_t) {}
}  // namespace LibraryFreshness

namespace BookDetailsLookup {
bool cached(const std::string&, uint32_t, BookDetails&) { return false; }
}  // namespace BookDetailsLookup

namespace CooperativeAbort {
bool shouldAbortLongTask() { return false; }
}  // namespace CooperativeAbort

namespace LibraryOrder {
bool sortBySeries(uint16_t*, size_t, BuildArena&, KeyFn, void*) { return false; }
}  // namespace LibraryOrder

// ---- Tests --------------------------------------------------------------------------------------

namespace {

class FileBrowserModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root = (fs::temp_directory_path() / ("file_browser_model_" + std::string(info->name()))).generic_string();
    fs::remove_all(root);
    fs::create_directories(root);
    FileIndex::failOpen = false;
  }
  void TearDown() override {
    fs::remove_all(root);
    FileIndex::failOpen = false;
    ESP.setFreeHeap(200 * 1024);
    ESP.setMaxAllocHeap(100 * 1024);
  }

  void file(const std::string& rel) const {
    const fs::path path = fs::path(root) / rel;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "x";
  }

  // `count` books with names the length a library's run to (40-odd bytes): long enough that each
  // std::string keeps its name in a heap block of its own, as on the device.
  void books(const size_t count) const {
    char name[96];
    for (size_t i = 0; i < count; ++i) {
      snprintf(name, sizeof(name), "Some Author - A Title Of Ordinary Length %04zu.epub", i);
      file(name);
    }
  }

  // The peak the shim's own listing of `path` costs: what a loop that only reads the directory needs.
  static long long plainListingPeak(const std::string& path) {
    const heap::Window window;
    {
      auto dir = Storage.open(path.c_str());
      char name[500];
      for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) entry.getName(name, sizeof(name));
    }
    return window.peakAbove();
  }

  std::string root;
};

// The bug: a folder over the index threshold was read whole into RAM first -- every name, size and
// date -- and only then handed to the SD index, which listed it again; the RAM copy was kept for as
// long as the folder was open. On the device that is ~92 bytes a book, and at the 513th the name
// vector asked for one 24 KB block: with ~60 KB free, five hundred books rebooted it.
TEST_F(FileBrowserModelTest, AnIndexedFolderKeepsNoNamesInRam) {
  books(400);
  FileBrowserModel model;
  model.setPath(root);

  const heap::Window window;
  model.load();

  ASSERT_TRUE(model.usesIndex());
  EXPECT_EQ(model.entryCount(), 400u);
  // The index object and what it keeps of the path, nothing per entry.
  EXPECT_LT(window.retained(), 2 * 1024);
}

TEST_F(FileBrowserModelTest, AnIndexedFolderIsNeverHeldWholeOnTheWay) {
  books(400);
  const long long listing = plainListingPeak(root);
  FileBrowserModel model;
  model.setPath(root);

  const heap::Window window;
  model.load();

  ASSERT_TRUE(model.usesIndex());
  // At most the threshold's worth of names before the folder turns out to be large: 64 of them are
  // ~6 KB here. All 400 were ~45 KB.
  EXPECT_LT(window.peakAbove() - listing, 12 * 1024);
}

TEST_F(FileBrowserModelTest, ASmallFolderIsListedInRamAndSorted) {
  file("b book 10.epub");
  file("b book 9.epub");
  file("a folder/x.epub");
  file("cover.jpg");  // not a book
  FileBrowserModel model;
  model.setPath(root);
  model.load();

  ASSERT_FALSE(model.usesIndex());
  ASSERT_EQ(model.entryCount(), 3u);
  EXPECT_EQ(model.entryName(0), "a folder/");  // folders first
  EXPECT_EQ(model.entryName(1), "b book 9.epub");
  EXPECT_EQ(model.entryName(2), "b book 10.epub");

  model.setSort(CrossPointSettings::SORT_BY_NAME, CrossPointSettings::SORT_DESCENDING);
  model.resort();
  EXPECT_EQ(model.entryName(0), "a folder/");
  EXPECT_EQ(model.entryName(1), "b book 10.epub");
  EXPECT_EQ(model.entryName(2), "b book 9.epub");
  EXPECT_EQ(model.findEntry("b book 9.epub"), 2u);
}

// A folder left -- the browser on the activity stack, or the Library switching tabs -- gives back
// what its listing took, capacity included.
TEST_F(FileBrowserModelTest, ClearGivesTheListingBack) {
  books(40);
  {
    // Once first, so the folder string load() keeps for the next one is not counted below.
    FileBrowserModel first;
    first.setPath(root);
    first.load();
  }
  FileBrowserModel model;
  model.setPath(root);

  const heap::Window window;
  model.load();
  ASSERT_EQ(model.entryCount(), 40u);
  model.clear();

  EXPECT_EQ(window.retained(), 0);
}

// Re-sorting moves the names into their new order rather than copying the listing, which held
// every name twice at once.
TEST_F(FileBrowserModelTest, ResortDoesNotCopyTheNames) {
  books(60);
  FileBrowserModel model;
  model.setPath(root);
  model.load();
  ASSERT_FALSE(model.usesIndex());
  ASSERT_EQ(model.entryName(0), "Some Author - A Title Of Ordinary Length 0000.epub");

  model.setSort(CrossPointSettings::SORT_BY_NAME, CrossPointSettings::SORT_DESCENDING);
  const heap::Window window;
  model.resort();

  // The sort's index of rows, nothing per name.
  EXPECT_LT(window.peakAbove(), static_cast<long long>(60 * sizeof(size_t) + 256));
  EXPECT_EQ(window.retained(), 0);
  EXPECT_EQ(model.entryName(0), "Some Author - A Title Of Ordinary Length 0059.epub");
  EXPECT_EQ(model.entryName(59), "Some Author - A Title Of Ordinary Length 0000.epub");
}

// A shuffled order -- cycles of every length -- with each row's size still its own book's.
TEST_F(FileBrowserModelTest, ResortKeepsEachNameWithItsSize) {
  char name[32];
  for (int i = 0; i < 50; ++i) {
    snprintf(name, sizeof(name), "book %02d.epub", i);
    std::ofstream(fs::path(root) / name, std::ios::binary) << std::string((i * 37) % 50 + 1, 'x');
  }
  FileBrowserModel model;
  model.setPath(root);
  model.setSort(CrossPointSettings::SORT_BY_SIZE, CrossPointSettings::SORT_ASCENDING);
  model.load();
  ASSERT_EQ(model.entryCount(), 50u);

  for (size_t row = 0; row < 50; ++row) {
    EXPECT_EQ(model.entrySize(row), row + 1);
    const int book = std::stoi(model.entryName(row).substr(5, 2));
    EXPECT_EQ(static_cast<size_t>((book * 37) % 50 + 1), model.entrySize(row)) << model.entryName(row);
  }
}

// The index could not be built (an SD write error): the folder is listed from RAM, all of it when
// the heap has room.
TEST_F(FileBrowserModelTest, WithoutTheIndexAFolderIsListedInRam) {
  books(300);
  FileIndex::failOpen = true;
  FileBrowserModel model;
  model.setPath(root);
  model.load();

  ASSERT_FALSE(model.usesIndex());
  ASSERT_EQ(model.entryCount(), 300u);
  EXPECT_EQ(model.entryName(0), "Some Author - A Title Of Ordinary Length 0000.epub");
  EXPECT_EQ(model.entryName(299), "Some Author - A Title Of Ordinary Length 0299.epub");
}

// ... and as much of it as the heap has room for when it has not, rather than aborting.
TEST_F(FileBrowserModelTest, WithoutTheIndexALowHeapShortensTheList) {
  books(300);
  FileIndex::failOpen = true;
  ESP.setFreeHeap(40 * 1024);
  FileBrowserModel model;
  model.setPath(root);
  model.load();

  ASSERT_FALSE(model.usesIndex());
  EXPECT_GT(model.entryCount(), 0u);
  EXPECT_LT(model.entryCount(), 300u);
  // Still sorted, whatever was left out.
  for (size_t i = 1; i < model.entryCount(); ++i) EXPECT_LT(model.entryName(i - 1), model.entryName(i));
}

TEST_F(FileBrowserModelTest, WithoutTheIndexAFragmentedHeapShortensTheList) {
  books(300);
  FileIndex::failOpen = true;
  ESP.setMaxAllocHeap(4 * 1024);  // the names' vector is one block
  FileBrowserModel model;
  model.setPath(root);
  model.load();

  EXPECT_GT(model.entryCount(), 0u);
  EXPECT_LT(model.entryCount(), 4 * 1024 / sizeof(std::string));
}

TEST_F(FileBrowserModelTest, WithoutTheIndexAHeapBelowTheFloorListsNothing) {
  books(300);
  FileIndex::failOpen = true;
  ESP.setFreeHeap(8 * 1024);
  FileBrowserModel model;
  model.setPath(root);
  model.load();

  EXPECT_EQ(model.entryCount(), 0u);
}

TEST_F(FileBrowserModelTest, SearchingEverywhereFindsBooksInFoldersBelow) {
  file("match one.epub");
  file("A/match two.epub");
  file("A/B/C/match three.epub");
  file("A/other.epub");
  file(".hidden/match hidden.epub");
  FileBrowserModel model;
  model.setPath(root);
  model.searchEverywhere("MATCH");

  ASSERT_TRUE(model.isDeepSearch());
  EXPECT_FALSE(model.deepResultsTruncated());
  ASSERT_EQ(model.entryCount(), 3u);
  std::vector<std::string> found;
  for (size_t i = 0; i < model.entryCount(); ++i) found.push_back(model.entryName(i));
  std::sort(found.begin(), found.end());
  EXPECT_EQ(found[0], "A/B/C/match three.epub");
  EXPECT_EQ(found[1], "A/match two.epub");
  EXPECT_EQ(found[2], "match one.epub");
}

// A card-wide search queued every folder it had still to visit, a path apiece: started above a
// library of a thousand author folders, that queue alone ran the heap out before a single folder
// below was read. Depth first, it holds one open folder per level.
TEST_F(FileBrowserModelTest, SearchingEverywhereHoldsOneFolderPerLevel) {
  char name[96];
  for (int i = 0; i < 600; ++i) {
    snprintf(name, sizeof(name), "An Author Called Somebody %03d/no match %03d.epub", i, i);
    file(name);
  }
  file("An Author Called Somebody 599/the one.epub");
  const long long listing = plainListingPeak(root);
  FileBrowserModel model;
  model.setPath(root);

  const heap::Window window;
  model.searchEverywhere("the one");

  ASSERT_EQ(model.entryCount(), 1u);
  EXPECT_EQ(model.entryName(0), "An Author Called Somebody 599/the one.epub");
  // 600 queued paths were ~70 KB.
  EXPECT_LT(window.peakAbove() - listing, 8 * 1024);
}

// Eight folders down, as far as the folder counts and the book index look.
TEST_F(FileBrowserModelTest, SearchingEverywhereLooksEightFoldersDown) {
  file("1/2/3/4/5/6/7/8/match eight.epub");
  file("1/2/3/4/5/6/7/8/9/match nine.epub");
  FileBrowserModel model;
  model.setPath(root);
  model.searchEverywhere("match");

  ASSERT_EQ(model.entryCount(), 1u);
  EXPECT_EQ(model.entryName(0), "1/2/3/4/5/6/7/8/match eight.epub");
}

TEST_F(FileBrowserModelTest, SearchingEverywhereStopsAtItsCap) {
  char name[64];
  for (int i = 0; i < 80; ++i) {
    snprintf(name, sizeof(name), "folder %02d/match %02d.epub", i, i);
    file(name);
  }
  FileBrowserModel model;
  model.setPath(root);
  model.searchEverywhere("match");

  EXPECT_EQ(model.entryCount(), 64u);
  EXPECT_TRUE(model.deepResultsTruncated());
}

}  // namespace

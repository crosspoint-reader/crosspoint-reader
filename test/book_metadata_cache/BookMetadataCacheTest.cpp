#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "ZipFile.h"

// Counting allocator: heap bytes allocated while `counting` is set and not yet freed.
namespace heap {
bool counting = false;
size_t live = 0;

struct alignas(std::max_align_t) Header {
  size_t size;
  bool counted;
};

void* allocate(const size_t size) {
  auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
  if (!header) return nullptr;
  header->size = size;
  header->counted = counting;
  if (counting) live += size;
  return header + 1;
}

void release(void* p) {
  if (!p) return;
  auto* header = static_cast<Header*>(p) - 1;
  if (header->counted) live -= header->size;
  std::free(header);
}
}  // namespace heap

void* operator new(const size_t size) {
  void* p = heap::allocate(size);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](const size_t size) { return operator new(size); }
void* operator new(const size_t size, const std::nothrow_t&) noexcept { return heap::allocate(size); }
void* operator new[](const size_t size, const std::nothrow_t&) noexcept { return heap::allocate(size); }
void operator delete(void* p) noexcept { heap::release(p); }
void operator delete[](void* p) noexcept { heap::release(p); }
void operator delete(void* p, size_t) noexcept { heap::release(p); }
void operator delete[](void* p, size_t) noexcept { heap::release(p); }

namespace {

std::string chapterHref(const int i) { return "OEBPS/Text/chapter" + std::to_string(i) + ".xhtml"; }
uint32_t chapterBytes(const int i) { return 1500 + static_cast<uint32_t>(i) * 7919 % 30000; }

std::string cacheDir() {
  return testing::TempDir() + "book_metadata_cache_" + testing::UnitTest::GetInstance()->current_test_info()->name();
}

// Writes book.bin through the cache's own writer; returns every chapter's cumulative size.
std::vector<uint32_t> buildBook(const std::string& dir, const int spineCount) {
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  zipEntrySizes.clear();
  std::vector<uint32_t> cumulative;
  for (int i = 0; i < spineCount; i++) {
    zipEntrySizes[chapterHref(i)] = chapterBytes(i);
    cumulative.push_back((i > 0 ? cumulative.back() : 0) + chapterBytes(i));
  }
  BookMetadataCache cache(dir);
  EXPECT_TRUE(cache.beginWrite() && cache.beginContentOpfPass());
  for (int i = 0; i < spineCount; i++) cache.createSpineEntry(chapterHref(i));
  EXPECT_TRUE(cache.endContentOpfPass() && cache.beginTocPass());
  for (int i = 0; i < spineCount; i++) cache.createTocEntry("Chapter " + std::to_string(i), chapterHref(i), "", 1);
  EXPECT_TRUE(cache.endTocPass() && cache.endWrite());
  EXPECT_TRUE(cache.buildBookBin("book.epub", {}));
  EXPECT_TRUE(cache.cleanupTmpFiles());
  return cumulative;
}

}  // namespace

// A loaded book must not hold heap per chapter: 5,000 chapters may hold at most 1 KB more than 30.
TEST(BookMetadataCacheHeap, ResidentHeapDoesNotGrowWithSpineCount) {
  size_t resident[2] = {};
  for (const int spineCount : {30, 5000}) {
    const std::vector<uint32_t> cumulative = buildBook(cacheDir(), spineCount);
    BookMetadataCache cache(cacheDir());
    heap::live = 0;
    heap::counting = true;
    EXPECT_TRUE(cache.load());
    const size_t loaded = heap::live;
    for (int i = 0; i < spineCount; i++) EXPECT_EQ(cache.getCumulativeSize(i), cumulative[i]) << "spine " << i;
    EXPECT_EQ(heap::live, loaded) << spineCount << " chapters";
    heap::counting = false;
    resident[spineCount == 5000] = loaded;
  }
  EXPECT_LE(resident[1], resident[0] + 1024) << "30 chapters: " << resident[0] << " B";
}

// book.bin cut short inside a window: the window answers the total before it, never less.
TEST(BookMetadataCacheSizes, TruncatedWindowFallsBackToTheTotalBeforeIt) {
  const std::vector<uint32_t> cumulative = buildBook(cacheDir(), 100);
  const std::string path = cacheDir() + "/book.bin";
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const size_t href = bytes.find(chapterHref(64));
  ASSERT_NE(href, std::string::npos);
  BookMetadataCache cache(cacheDir());
  ASSERT_TRUE(cache.load());
  EXPECT_EQ(cache.getCumulativeSize(0), cumulative[0]);
  std::filesystem::resize_file(path, href + chapterHref(64).size());  // just before item 64's size
  EXPECT_EQ(cache.getCumulativeSize(63), cumulative[63]);
  EXPECT_EQ(cache.getCumulativeSize(64), cumulative[63]);
}

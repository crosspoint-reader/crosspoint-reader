#include <BookMetadataCache.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstring>
namespace {
constexpr const char* CACHE = "/.crosspoint/test";
void build(BookMetadataCache& cache) {
  fake::reset();
  fake::add("/book.txt");
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  cache.createSpineEntry("text.html");
  ASSERT_TRUE(cache.endContentOpfPass());
  ASSERT_TRUE(cache.beginTocPass());
  cache.createTocEntry("Chapter", "text.html", "anchor", 0);
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());
  ASSERT_TRUE(cache.buildBookBin("/book.txt", {"Title", "Author", "en", "", "text.html"}));
}
}  // namespace
TEST(BookMetadataCache, ValidCacheRoundTrips) {
  BookMetadataCache cache(CACHE);
  build(cache);
  ASSERT_TRUE(cache.load());
  EXPECT_EQ(cache.coreMetadata.title, "Title");
  EXPECT_EQ(cache.getSpineEntry(0).href, "text.html");
  EXPECT_EQ(cache.getTocEntry(0).anchor, "anchor");
}
TEST(BookMetadataCache, OversizedOrTruncatedMetadataIsRejected) {
  for (bool truncated : {false, true}) {
    BookMetadataCache cache(CACHE);
    build(cache);
    auto& bytes = fake::files.at(std::string(CACHE) + "/book.bin")->bytes;
    if (truncated)
      bytes.resize(11);
    else {
      const uint32_t huge = 0x10000000;
      std::memcpy(bytes.data() + 9, &huge, 4);
    }
    EXPECT_FALSE(cache.load());
    EXPECT_FALSE(cache.isLoaded());
    EXPECT_EQ(cache.getSpineCount(), 0);
  }
}
TEST(BookMetadataCache, BadLookupAndTruncatedTocAreRejected) {
  for (bool truncated : {false, true}) {
    BookMetadataCache cache(CACHE);
    build(cache);
    auto& bytes = fake::files.at(std::string(CACHE) + "/book.bin")->bytes;
    if (truncated)
      bytes.pop_back();
    else {
      uint32_t lut;
      std::memcpy(&lut, bytes.data() + 1, 4);
      const uint32_t bad = UINT32_MAX;
      std::memcpy(bytes.data() + lut, &bad, 4);
    }
    EXPECT_FALSE(cache.load());
  }
}
TEST(BookMetadataCache, CumulativeAllocationFailureDoesNotPublishCache) {
  BookMetadataCache cache(CACHE);
  build(cache);
  fake::failAlloc = 0;
  EXPECT_FALSE(cache.load());
  EXPECT_FALSE(cache.isLoaded());
}

TEST(BookMetadataCache, MissingCacheClearsPreviouslyLoadedCounts) {
  BookMetadataCache cache(CACHE);
  build(cache);
  ASSERT_TRUE(cache.load());
  ASSERT_TRUE(Storage.remove((std::string(CACHE) + "/book.bin").c_str()));
  EXPECT_FALSE(cache.load());
  EXPECT_EQ(cache.getSpineCount(), 0);
  EXPECT_EQ(cache.getTocCount(), 0);
}

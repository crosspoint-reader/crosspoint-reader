#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "FlashBlobCache.h"

// Fake NOR partition for FlashBlobCache's backend: erase sets bytes to 0xFF,
// writes can only clear bits, and map() hands out pointers into the array.
// CMake builds this file once per partition size; EXPECTED_SLOTS is how many
// slots the cache should make of it (0: too small to use).
namespace fake {
constexpr uint32_t kSize = FAKE_FLASH_SIZE;
std::vector<uint8_t> flash(kSize, 0xFF);
int erases = 0;
int writes = 0;
uint32_t nextHandle = 1;
std::map<uint32_t, uint32_t> maps;  // handle -> offset

void reset() {
  std::fill(flash.begin(), flash.end(), 0xFF);
  erases = writes = 0;
  maps.clear();
}
}  // namespace fake

namespace FlashBlobCache::backend {
bool open(uint32_t* size) {
  *size = fake::kSize;
  return true;
}
bool read(const uint32_t offset, void* buf, const uint32_t length) {
  if (offset + length > fake::kSize) return false;
  std::memcpy(buf, fake::flash.data() + offset, length);
  return true;
}
bool erase(const uint32_t offset, const uint32_t length) {
  EXPECT_EQ(offset % 4096, 0u);
  EXPECT_EQ(length % 4096, 0u);
  if (offset + length > fake::kSize) return false;
  std::memset(fake::flash.data() + offset, 0xFF, length);
  fake::erases++;
  return true;
}
bool write(const uint32_t offset, const void* buf, const uint32_t length) {
  if (offset + length > fake::kSize) return false;
  const auto* src = static_cast<const uint8_t*>(buf);
  for (uint32_t i = 0; i < length; i++) fake::flash[offset + i] &= src[i];
  fake::writes++;
  return true;
}
const uint8_t* map(const uint32_t offset, const uint32_t length, uint32_t* handle) {
  if (offset + length > fake::kSize) return nullptr;
  *handle = fake::nextHandle++;
  fake::maps[*handle] = offset;
  return fake::flash.data() + offset;
}
void unmap(const uint32_t handle) { EXPECT_EQ(fake::maps.erase(handle), 1u); }
}  // namespace FlashBlobCache::backend

namespace {

uint32_t keyOf(const std::vector<uint8_t>& blob) {
  uint32_t hash = 2166136261u;
  for (const uint8_t b : blob) {
    hash ^= b;
    hash *= 16777619u;
  }
  return hash | 1u;
}

std::vector<uint8_t> makeBlob(const uint32_t length, const uint8_t seed) {
  std::vector<uint8_t> blob(length);
  for (uint32_t i = 0; i < length; i++) blob[i] = static_cast<uint8_t>(seed + i * 7);
  return blob;
}

int gReads = 0;
bool gFailReads = false;

bool readBlob(void* ctx, const uint32_t offset, uint8_t* buf, const uint32_t length) {
  gReads++;
  if (gFailReads) return false;
  const auto* blob = static_cast<const std::vector<uint8_t>*>(ctx);
  std::memcpy(buf, blob->data() + offset, length);
  return true;
}

class FlashBlobCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    gReads = 0;
    gFailReads = false;
  }
  void TearDown() override {
    for (const uint8_t* p : live) FlashBlobCache::release(const_cast<uint8_t*>(p));
    EXPECT_TRUE(fake::maps.empty());
  }

  const uint8_t* acquire(const std::vector<uint8_t>& blob) {
    const uint8_t* p = FlashBlobCache::acquire(keyOf(blob), static_cast<uint32_t>(blob.size()), readBlob,
                                               const_cast<std::vector<uint8_t>*>(&blob));
    if (p != nullptr) live.push_back(p);
    return p;
  }
  void release(const uint8_t* p) {
    FlashBlobCache::release(const_cast<uint8_t*>(p));
    live.erase(std::find(live.begin(), live.end(), p));
  }

  std::vector<const uint8_t*> live;
};

}  // namespace

#if EXPECTED_SLOTS == 0

TEST_F(FlashBlobCacheTest, DeclinesAPartitionTooSmallForTwoSlots) {
  const auto blob = makeBlob(2000, 1);
  EXPECT_FALSE(FlashBlobCache::canHold(keyOf(blob), static_cast<uint32_t>(blob.size())));
  EXPECT_EQ(acquire(blob), nullptr);
  EXPECT_EQ(fake::erases, 0);
  EXPECT_EQ(fake::writes, 0);
}

#else

TEST_F(FlashBlobCacheTest, CopiesOnFirstUseAndMapsTheCopyAfterwards) {
  const auto blob = makeBlob(36736, 1);
  const uint8_t* first = acquire(blob);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(std::memcmp(first, blob.data(), blob.size()), 0);
  EXPECT_GT(gReads, 0);
  release(first);

  gReads = 0;
  const int writesBefore = fake::writes;
  const uint8_t* second = acquire(blob);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(std::memcmp(second, blob.data(), blob.size()), 0);
  EXPECT_EQ(gReads, 0) << "a cached blob must not be read from SD again";
  EXPECT_EQ(fake::writes, writesBefore) << "a cached blob must not be rewritten";
}

TEST_F(FlashBlobCacheTest, KeepsDistinctBlobsInDistinctSlots) {
  const auto a = makeBlob(20000, 1), b = makeBlob(30000, 2);
  const uint8_t* pa = acquire(a);
  const uint8_t* pb = acquire(b);
  ASSERT_NE(pa, nullptr);
  ASSERT_NE(pb, nullptr);
  EXPECT_NE(pa, pb);
  EXPECT_EQ(std::memcmp(pa, a.data(), a.size()), 0);
  EXPECT_EQ(std::memcmp(pb, b.data(), b.size()), 0);
}

TEST_F(FlashBlobCacheTest, RecopiesACorruptedSlot) {
  const auto blob = makeBlob(4096, 3);
  const uint8_t* p = acquire(blob);
  ASSERT_NE(p, nullptr);
  const size_t offset = static_cast<size_t>(p - fake::flash.data());
  release(p);
  fake::flash[offset + 100] ^= 0x5A;  // bit rot

  gReads = 0;
  const uint8_t* again = acquire(blob);
  ASSERT_NE(again, nullptr);
  EXPECT_GT(gReads, 0);
  EXPECT_EQ(std::memcmp(again, blob.data(), blob.size()), 0);
}

TEST_F(FlashBlobCacheTest, DoesNotRewriteFlashForABlobThatFailsItsHash) {
  const auto blob = makeBlob(4096, 6);
  const uint32_t wrongKey = keyOf(blob) ^ 0x100;  // e.g. a corrupt hash in the .cpfont header
  EXPECT_EQ(FlashBlobCache::acquire(wrongKey, static_cast<uint32_t>(blob.size()), readBlob,
                                    const_cast<std::vector<uint8_t>*>(&blob)),
            nullptr);
  const int erasesAfterFirst = fake::erases;
  EXPECT_GT(erasesAfterFirst, 0);

  gReads = 0;
  EXPECT_EQ(FlashBlobCache::acquire(wrongKey, static_cast<uint32_t>(blob.size()), readBlob,
                                    const_cast<std::vector<uint8_t>*>(&blob)),
            nullptr);
  EXPECT_EQ(fake::erases, erasesAfterFirst) << "a rejected blob must not erase a slot again";
  EXPECT_EQ(gReads, 0);
}

TEST_F(FlashBlobCacheTest, RecyclesTheOldestIdleSlotButNeverAMappedOne) {
  std::vector<std::vector<uint8_t> > blobs;
  for (uint8_t i = 0; i <= EXPECTED_SLOTS; i++) blobs.push_back(makeBlob(1000 + i, static_cast<uint8_t>(10 + i)));
  const uint8_t* pinned = acquire(blobs[0]);  // stays mapped throughout
  ASSERT_NE(pinned, nullptr);
  for (int i = 1; i < EXPECTED_SLOTS; i++) {
    gReads = 0;
    const uint8_t* p = acquire(blobs[i]);
    ASSERT_NE(p, nullptr);
    EXPECT_GT(gReads, 0) << "blob " << i << " should have been copied into a free slot";
    release(p);
  }
  // Every slot is used; the next blob must evict slot 1 (oldest idle), not slot 0.
  const uint8_t* extra = acquire(blobs[EXPECTED_SLOTS]);
  ASSERT_NE(extra, nullptr);
  EXPECT_EQ(std::memcmp(pinned, blobs[0].data(), blobs[0].size()), 0);
  EXPECT_EQ(std::memcmp(extra, blobs[EXPECTED_SLOTS].data(), blobs[EXPECTED_SLOTS].size()), 0);

  gReads = 0;
  const uint8_t* evicted = acquire(blobs[1]);
  ASSERT_NE(evicted, nullptr);
  EXPECT_GT(gReads, 0) << "blob 1 was the oldest idle entry and should have been recycled";
}

TEST_F(FlashBlobCacheTest, DeclinesWhatItCannotHold) {
  const auto fits = makeBlob(FlashBlobCache::MAX_BLOB_BYTES, 3);
  const auto tooLarge = makeBlob(FlashBlobCache::MAX_BLOB_BYTES + 1, 4);
  EXPECT_TRUE(FlashBlobCache::canHold(keyOf(fits), static_cast<uint32_t>(fits.size())));
  EXPECT_FALSE(FlashBlobCache::canHold(keyOf(tooLarge), static_cast<uint32_t>(tooLarge.size())));
  EXPECT_NE(acquire(fits), nullptr);      // fills a slot exactly
  EXPECT_EQ(acquire(tooLarge), nullptr);  // larger than a slot

  const auto blob = makeBlob(2000, 5);
  gFailReads = true;
  EXPECT_EQ(acquire(blob), nullptr);  // SD read failed mid-copy
  gFailReads = false;
  // Each copy erases a slot first, so a failed one is not retried.
  EXPECT_FALSE(FlashBlobCache::canHold(keyOf(blob), static_cast<uint32_t>(blob.size())));
  const int erases = fake::erases;
  gReads = 0;
  EXPECT_EQ(acquire(blob), nullptr);
  EXPECT_EQ(gReads, 0);
  EXPECT_EQ(fake::erases, erases);
  // Other blobs still copy.
  EXPECT_NE(acquire(makeBlob(2000, 7)), nullptr);
}

#endif

// HalFile handles from lib/hal/HalStorage.cpp, built as the firmware builds it, over SdFat on an
// in-memory card. Whatever an open returns, its handle may be closed: an open that fails holds the
// Impl all failed opens share, costing no heap, and an open whose own Impl cannot be had fails like
// any other instead of aborting the device. A handle never opened keeps its assert.
// As in HalStorage.cpp: FsFile is SdFat's here, which the test mounts and writes through.
#define HAL_STORAGE_IMPL
#include <HalClock.h>
#include <HalSpiBus.h>
#include <HalStorage.h>
#include <SDCardManager.h>
#include <SdFat.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>

// --- What HalStorage.cpp links against ---------------------------------------------------------

HalSpiBus::Lock::Lock() {}
HalSpiBus::Lock::~Lock() {}

namespace HalClock {
bool isSynced() { return false; }
time_t now() { return 0; }
}  // namespace HalClock

// The heap: every allocation counted, and the nothrow ones refused on demand. Only those: a
// throwing new that fails aborts the device (-fno-exceptions), so the HAL must not rely on one.
namespace {
bool refuseNothrowNew = false;
size_t heapNews = 0;
}  // namespace

void* operator new(const std::size_t size) {
  ++heapNews;
  if (void* block = std::malloc(size != 0 ? size : 1)) return block;
  throw std::bad_alloc();
}
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  if (refuseNothrowNew) return nullptr;
  ++heapNews;
  return std::malloc(size != 0 ? size : 1);
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }

namespace {

// Keeps only the sectors that are not all zero.
class RamCard final : public FsBlockDeviceInterface {
 public:
  explicit RamCard(const Sector_t sectors) : sectors_(sectors) {}

  bool isBusy() override { return false; }
  bool readSector(const Sector_t sector, uint8_t* dst) override {
    if (sector >= sectors_) return false;
    const auto it = data_.find(sector);
    if (it == data_.end()) {
      memset(dst, 0, 512);
    } else {
      memcpy(dst, it->second.data(), 512);
    }
    return true;
  }
  bool readSectors(const Sector_t sector, uint8_t* dst, const size_t count) override {
    for (size_t i = 0; i < count; ++i) {
      if (!readSector(sector + i, dst + 512 * i)) return false;
    }
    return true;
  }
  Sector_t sectorCount() override { return sectors_; }
  bool syncDevice() override { return true; }
  bool writeSector(const Sector_t sector, const uint8_t* src) override {
    if (sector >= sectors_) return false;
    memcpy(data_[sector].data(), src, 512);
    return true;
  }
  bool writeSectors(const Sector_t sector, const uint8_t* src, const size_t count) override {
    for (size_t i = 0; i < count; ++i) {
      if (!writeSector(sector + i, src + 512 * i)) return false;
    }
    return true;
  }

 private:
  Sector_t sectors_;
  std::unordered_map<Sector_t, std::array<uint8_t, 512>> data_;
};

class HalStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    uint8_t buffer[512];
    FatFormatter formatter;
    ASSERT_TRUE(formatter.format(&card_, buffer, nullptr));
    ASSERT_TRUE(vol_.begin(&card_));
    SDCardManager::getInstance().vol = &vol_;
    ASSERT_TRUE(Storage.begin());
    refuseNothrowNew = false;
    heapNews = 0;
  }
  void TearDown() override {
    refuseNothrowNew = false;
    EXPECT_EQ(hostSemaphoreDepth(), 0) << "a StorageLock was not given back";
    SDCardManager::getInstance().vol = nullptr;
  }

  // A file of `text` at `path`, written straight through SdFat.
  void put(const char* path, const std::string& text) {
    FsFile file = vol_.open(path, O_RDWR | O_CREAT | O_TRUNC);
    ASSERT_TRUE(file);
    ASSERT_EQ(file.write(text.data(), text.size()), text.size());
    ASSERT_TRUE(file.close());
  }

  RamCard card_{64 * 2048};  // 64 MB
  FsVolume vol_;
};

TEST_F(HalStorageTest, AFileThatOpensHoldsAnImplOfItsOwn) {
  put("/book.txt", "page");
  HalFile file;
  heapNews = 0;

  ASSERT_TRUE(Storage.openFileForRead("TEST", "/book.txt", file));
  EXPECT_EQ(heapNews, 1u);
  char text[8] = {};
  EXPECT_EQ(file.read(text, sizeof(text)), 4);
  EXPECT_STREQ(text, "page");
  EXPECT_TRUE(file.close());
}

TEST_F(HalStorageTest, AFailedOpenTakesNoHeapAndMayBeClosed) {
  HalFile opened = Storage.open("/missing.txt");
  HalFile read;
  HalFile updated;

  EXPECT_FALSE(opened);
  EXPECT_FALSE(Storage.openFileForRead("TEST", "/missing.txt", read));
  EXPECT_FALSE(read);
  EXPECT_FALSE(Storage.openFileForUpdate("TEST", "/missing.txt", updated));
  EXPECT_EQ(heapNews, 0u);
  // As on a closed file: nothing to read, nothing written, and close() is allowed.
  char byte = 0;
  EXPECT_EQ(opened.read(&byte, 1), -1);
  EXPECT_EQ(read.write("x", 1), 0u);
  opened.close();
  read.close();
  updated.close();
}

TEST_F(HalStorageTest, TheEndOfAListingTakesNoHeap) {
  ASSERT_TRUE(vol_.mkdir("/books"));
  put("/books/a.txt", "a");
  put("/books/b.txt", "b");
  HalFile folder = Storage.open("/books");
  ASSERT_TRUE(folder);
  heapNews = 0;

  int listed = 0;
  for (HalFile entry = folder.openNextFile(); entry; entry = folder.openNextFile()) ++listed;

  EXPECT_EQ(listed, 2);
  EXPECT_EQ(heapNews, 2u);  // the entries; the end of the listing took none
}

// What used to abort the device: std::make_unique under -fno-exceptions.
TEST_F(HalStorageTest, AnOpenWhoseImplCannotBeHadFailsAndMayBeClosed) {
  put("/book.txt", "page");
  ASSERT_TRUE(vol_.mkdir("/books"));
  put("/books/a.txt", "a");
  HalFile folder = Storage.open("/books");
  ASSERT_TRUE(folder);
  refuseNothrowNew = true;

  HalFile opened = Storage.open("/book.txt");
  HalFile read;
  HalFile written;
  EXPECT_FALSE(opened);
  EXPECT_FALSE(Storage.openFileForRead("TEST", "/book.txt", read));
  EXPECT_FALSE(read);
  EXPECT_FALSE(Storage.openFileForWrite("TEST", "/new.txt", written));
  EXPECT_FALSE(written);
  EXPECT_FALSE(folder.openNextFile());  // ends the listing
  EXPECT_EQ(written.write("x", 1), 0u);
  opened.close();
  read.close();
  written.close();

  // The files the failed opens had open were closed: once there is heap again, all open as before.
  refuseNothrowNew = false;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/book.txt", read));
  char text[8] = {};
  EXPECT_EQ(read.read(text, sizeof(text)), 4);
  EXPECT_STREQ(text, "page");
  EXPECT_TRUE(Storage.exists("/new.txt"));
}

TEST_F(HalStorageTest, ManyFailedOpensShareOneImplAndNeverFreeIt) {
  for (int i = 0; i < 1000; ++i) {
    HalFile file = Storage.open("/missing.txt");
    HalFile moved = std::move(file);
    file = Storage.open("/missing-too.txt");
    EXPECT_FALSE(moved);
    EXPECT_FALSE(file);
  }
  EXPECT_EQ(heapNews, 0u);
}

// The tripwire stays: a handle no open ever filled is a caller's bug.
TEST_F(HalStorageTest, ClosingAHandleNeverOpenedStillAsserts) {
  EXPECT_DEATH(
      {
        HalFile never;
        never.close();
      },
      "impl != nullptr");
}

}  // namespace

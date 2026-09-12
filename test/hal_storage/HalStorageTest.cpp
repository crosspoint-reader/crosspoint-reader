#include <HalStorage.h>
#include <StorageTestSupport.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <new>

namespace {

// Armed only around a HAL call; the fixed-size SDK stub never allocates.
// Unexpected throwing allocations are counted while the probe is armed. An
// actual malloc failure still aborts, matching the firmware's no-exceptions
// behavior. CTest runs each case separately, including baseline failures.
class AllocationProbe {
 public:
  explicit AllocationProbe(bool fail = false) {
    storage_test::state.allocations = 0;
    storage_test::state.nothrowAllocations = 0;
    storage_test::state.throwingAllocations = 0;
    storage_test::state.countAllocations = true;
    storage_test::state.failAllocation = fail;
  }
  ~AllocationProbe() {
    storage_test::state.countAllocations = false;
    storage_test::state.failAllocation = false;
    EXPECT_EQ(storage_test::state.throwingAllocations, 0U);
  }
};

class HalStorageTest : public ::testing::Test {
 protected:
  void SetUp() override { storage_test::state = {}; }
  void TearDown() override {
    const auto& state = storage_test::state;
    EXPECT_EQ(state.activeHandles, 0U);
    EXPECT_EQ(state.lockDepth, 0);
    EXPECT_EQ(state.lockTakes, state.lockGives);
    EXPECT_FALSE(state.unlockedOperation);
  }
};

TEST_F(HalStorageTest, OpensAndDestroysFileUnderLock) {
  {
    HalFile file;
    {
      AllocationProbe probe;
      file = Storage.open("/book.epub");
    }
    EXPECT_TRUE(file);
    EXPECT_EQ(storage_test::state.activeHandles, 1U);
    EXPECT_EQ(storage_test::state.allocations, 1U);
  }
  EXPECT_EQ(storage_test::state.closeCalls, 1U);
}

TEST_F(HalStorageTest, MissingFileDoesNotAllocateWrapper) {
  storage_test::state.openSucceeds = false;
  HalFile file;
  {
    AllocationProbe probe;
    file = Storage.open("/missing");
  }
  EXPECT_FALSE(file);
  EXPECT_EQ(storage_test::state.allocations, 0U);
}

TEST_F(HalStorageTest, FailedReadClearsPreviousHandleWithoutAllocation) {
  auto file = Storage.open("/previous");
  storage_test::state.openSucceeds = false;
  bool result;
  {
    AllocationProbe probe;
    result = Storage.openFileForRead("TEST", "/missing", file);
  }
  EXPECT_FALSE(result);
  EXPECT_FALSE(file);
  EXPECT_EQ(storage_test::state.allocations, 0U);
  EXPECT_EQ(storage_test::state.closeCalls, 1U);
  EXPECT_GE(storage_test::state.maxLockDepth, 2);
}

TEST_F(HalStorageTest, FailedWriteClearsPreviousHandleWithoutAllocation) {
  auto file = Storage.open("/previous");
  storage_test::state.openSucceeds = false;
  bool result;
  {
    AllocationProbe probe;
    result = Storage.openFileForWrite("TEST", "/unwritable", file);
  }
  EXPECT_FALSE(result);
  EXPECT_FALSE(file);
  EXPECT_EQ(storage_test::state.allocations, 0U);
  EXPECT_EQ(storage_test::state.closeCalls, 1U);
  EXPECT_GE(storage_test::state.maxLockDepth, 2);
}

TEST_F(HalStorageTest, DirectoryEndDoesNotAllocateWrapper) {
  auto directory = Storage.open("/dir");
  HalFile entry;
  {
    AllocationProbe probe;
    entry = directory.openNextFile();
  }
  EXPECT_FALSE(entry);
  EXPECT_EQ(storage_test::state.allocations, 0U);
  EXPECT_EQ(storage_test::state.activeHandles, 1U);
}

TEST_F(HalStorageTest, OpenAllocationFailureClosesFile) {
  HalFile file;
  {
    AllocationProbe probe(true);
    file = Storage.open("/book.epub");
  }
  EXPECT_FALSE(file);
  EXPECT_EQ(storage_test::state.nothrowAllocations, 1U);
  EXPECT_EQ(storage_test::state.errors, 1U);
  EXPECT_EQ(storage_test::state.closeCalls, 1U);
}

TEST_F(HalStorageTest, ReadAllocationFailureClosesBothHandles) {
  auto file = Storage.open("/previous");
  bool result;
  {
    AllocationProbe probe(true);
    result = Storage.openFileForRead("TEST", "/book.epub", file);
  }
  EXPECT_FALSE(result);
  EXPECT_FALSE(file);
  EXPECT_EQ(storage_test::state.nothrowAllocations, 1U);
  EXPECT_EQ(storage_test::state.closeCalls, 2U);
  EXPECT_GE(storage_test::state.maxLockDepth, 2);
}

TEST_F(HalStorageTest, WriteAllocationFailureClosesBothHandles) {
  auto file = Storage.open("/previous");
  bool result;
  {
    AllocationProbe probe(true);
    result = Storage.openFileForWrite("TEST", "/new.epub", file);
  }
  EXPECT_FALSE(result);
  EXPECT_FALSE(file);
  EXPECT_EQ(storage_test::state.nothrowAllocations, 1U);
  EXPECT_EQ(storage_test::state.closeCalls, 2U);
  EXPECT_GE(storage_test::state.maxLockDepth, 2);
}

TEST_F(HalStorageTest, NextAllocationFailureClosesEntryButKeepsDirectory) {
  auto directory = Storage.open("/dir");
  storage_test::state.nextEntries = 1;
  HalFile entry;
  {
    AllocationProbe probe(true);
    entry = directory.openNextFile();
  }
  EXPECT_FALSE(entry);
  EXPECT_TRUE(directory);
  EXPECT_EQ(storage_test::state.nothrowAllocations, 1U);
  EXPECT_EQ(storage_test::state.activeHandles, 1U);
  EXPECT_EQ(storage_test::state.closeCalls, 1U);
}

TEST_F(HalStorageTest, EmptyHandleCanBeClosed) {
  HalFile file;
  EXPECT_TRUE(file.close());
  EXPECT_FALSE(file);
  EXPECT_EQ(storage_test::state.closeCalls, 0U);
}

TEST_F(HalStorageTest, MovedFromHandleCanBeClosed) {
  auto file = Storage.open("/book.epub");
  HalFile moved = std::move(file);
  EXPECT_TRUE(file.close());
  EXPECT_TRUE(moved);
  EXPECT_EQ(storage_test::state.closeCalls, 0U);
}

TEST_F(HalStorageTest, MissingOptionalSidecarCanBeClosedDuringCleanup) {
  auto synonyms = Storage.open("/dictionary.syn");
  storage_test::state.openSucceeds = false;
  HalFile sidecar;
  EXPECT_FALSE(Storage.openFileForRead("DICT", "/dictionary.sidx", sidecar));
  // Dictionary::openSynonyms closes both handles when the optional index
  // cannot be opened. The missing sidecar must not prevent normal cleanup.
  EXPECT_TRUE(sidecar.close());
  EXPECT_TRUE(synonyms.close());
  EXPECT_EQ(storage_test::state.closeCalls, 1U);
}

TEST_F(HalStorageTest, CloseReportsUnderlyingFailureAndRemainsIdempotent) {
  auto file = Storage.open("/book.epub");
  storage_test::state.closeSucceeds = false;
  EXPECT_FALSE(file.close());
  EXPECT_FALSE(file);
  EXPECT_TRUE(file.close());
  EXPECT_EQ(storage_test::state.closeCalls, 1U);
}

TEST_F(HalStorageTest, SuccessfulReadWriteAndNextTransferOwnership) {
  HalFile file;
  EXPECT_TRUE(Storage.openFileForRead("TEST", "/book.epub", file));
  EXPECT_TRUE(file);
  EXPECT_TRUE(Storage.openFileForWrite("TEST", "/new.epub", file));
  EXPECT_TRUE(file);
  EXPECT_EQ(storage_test::state.closeCalls, 1U);
  auto directory = Storage.open("/dir");
  storage_test::state.nextEntries = 1;
  auto entry = directory.openNextFile();
  EXPECT_TRUE(entry);
  EXPECT_EQ(storage_test::state.activeHandles, 3U);
}

}  // namespace

void* operator new(const size_t size) {
  auto& state = storage_test::state;
  if (state.countAllocations) {
    ++state.allocations;
    ++state.throwingAllocations;
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  std::abort();
}

void* operator new(const size_t size, const std::nothrow_t&) noexcept {
  auto& state = storage_test::state;
  if (state.countAllocations) {
    ++state.allocations;
    ++state.nothrowAllocations;
    if (state.failAllocation) return nullptr;
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

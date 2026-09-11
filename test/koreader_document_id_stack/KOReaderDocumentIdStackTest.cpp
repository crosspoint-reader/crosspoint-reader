#include <gtest/gtest.h>

#include <cstddef>
#include <new>
#include <string>
#include <vector>

#include "Fixture.h"
#include "KOReaderDocumentId.h"

namespace {
constexpr bool CAN_WRAP_NOTHROW = DOCUMENT_ID_STACK_WRAP_NOTHROW != 0;
bool failScratchAllocation = false;
size_t scratchAllocationCalls = 0;
}  // namespace

#if DOCUMENT_ID_STACK_WRAP_NOTHROW
// The Linux 64-bit Itanium ABI symbol for operator new[](size_t, nothrow_t).
// The linker redirects only this overload; successful allocations and delete[]
// continue to use the original runtime. Memory.h itself is compiled unchanged.
extern "C" void* __real__ZnamRKSt9nothrow_t(size_t, const std::nothrow_t&) noexcept;
extern "C" void* __wrap__ZnamRKSt9nothrow_t(const size_t size, const std::nothrow_t& tag) noexcept {
  if (size == 1024) {
    ++scratchAllocationCalls;
    if (failScratchAllocation) return nullptr;
  }
  return __real__ZnamRKSt9nothrow_t(size, tag);
}
#endif

namespace {

class DocumentIdStack : public testing::Test {
 protected:
  void SetUp() override {
    documentIdFixture::state = {};
    failScratchAllocation = false;
    scratchAllocationCalls = 0;
  }
  void TearDown() override { failScratchAllocation = false; }
};

TEST_F(DocumentIdStack, PreservesIndependentPartialMd5VectorsAcrossSampleBoundaries) {
  struct Vector {
    size_t size;
    const char* expected;
  };
  // Python hashlib over the official samples of the deterministic fixture.
  constexpr Vector vectors[] = {
      {0, "d41d8cd98f00b204e9800998ecf8427e"},          {3, "d62478a52246cec25989b858b9b222ee"},
      {1023, "7f1c37f5e8fd6ae22008b53d030d07c3"},       {1024, "ddc19a518eeb3773185ab4ec0be23559"},
      {1025, "a7731fd4faf04ace72dec76b627f5541"},       {4096, "a1f6456f1c8a9cb51533c1dafc3a4560"},
      {4097, "ca8fd68bfafea71a915c421dfb3f18a3"},       {163840, "12ddd5ce3224e2917e46bac2380b6940"},
      {1073742000, "2319b60b7cacc174f4a61055f11a966d"},
  };
  for (const auto& vector : vectors) {
    SCOPED_TRACE(vector.size);
    documentIdFixture::state = {};
    documentIdFixture::state.fileSize = vector.size;
    EXPECT_EQ(KOReaderDocumentId::calculate("/fixture/book.epub"), vector.expected);
    EXPECT_FALSE(documentIdFixture::state.oversizedHashFeed);
    EXPECT_EQ(documentIdFixture::state.closedFiles, 1);
  }
}

TEST_F(DocumentIdStack, SamplesAllOfficialOffsets) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1073742000;
  ASSERT_FALSE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  if (CAN_WRAP_NOTHROW) {
    EXPECT_EQ(scratchAllocationCalls, 1U);
  }
  EXPECT_EQ(fixture.seeks, (std::vector<size_t>{0, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216,
                                                67108864, 268435456, 1073741824}));
  EXPECT_EQ(fixture.hashFeedSizes.back(), 176U);
}

TEST_F(DocumentIdStack, AllocationFailureReturnsNoHashAndClosesTheFile) {
  if (!CAN_WRAP_NOTHROW) {
    GTEST_SKIP() << "Nothrow array allocation wrapping is unavailable or disabled";
  }
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 4097;
  failScratchAllocation = true;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  failScratchAllocation = false;
  EXPECT_EQ(scratchAllocationCalls, 1U);
  EXPECT_TRUE(fixture.seeks.empty());
  EXPECT_TRUE(fixture.reads.empty());
  EXPECT_TRUE(fixture.hashFeedSizes.empty());
  EXPECT_EQ(fixture.closedFiles, 1);
}

TEST_F(DocumentIdStack, OpenFailureDoesNotAllocateScratchStorage) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1024;
  fixture.openSucceeds = false;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/missing.epub").empty());
  if (CAN_WRAP_NOTHROW) {
    EXPECT_EQ(scratchAllocationCalls, 0U);
  }
  EXPECT_TRUE(fixture.reads.empty());
  EXPECT_EQ(fixture.closedFiles, 0);
}

TEST_F(DocumentIdStack, FilenameHashingDoesNotDependOnScratchStorage) {
  failScratchAllocation = true;
  EXPECT_EQ(KOReaderDocumentId::calculateFromFilename("/one/book.epub"), "03053ffc045564439ff7f2cabb3b58c5");
  EXPECT_EQ(KOReaderDocumentId::calculateFromFilename("/another/book.epub"), "03053ffc045564439ff7f2cabb3b58c5");
  EXPECT_TRUE(KOReaderDocumentId::calculateFromFilename("/directory/").empty());
  if (CAN_WRAP_NOTHROW) {
    EXPECT_EQ(scratchAllocationCalls, 0U);
  }
}

}  // namespace

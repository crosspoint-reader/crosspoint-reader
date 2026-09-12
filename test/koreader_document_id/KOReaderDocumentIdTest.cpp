#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "Fixture.h"
#include "KOReaderDocumentId.h"

namespace {

class DocumentId : public testing::Test {
 protected:
  void SetUp() override { documentIdFixture::state = {}; }
};

TEST_F(DocumentId, PreservesIndependentPartialMd5VectorsAcrossSampleBoundaries) {
  struct Vector {
    size_t size;
    const char* expected;
  };
  // Pattern: byte(position) = (position * 37 + position / 251) % 256.
  // Expected digests were independently calculated with Python hashlib over
  // the concatenated 1024-byte samples at the official offsets.
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

TEST_F(DocumentId, ReadsAllOfficialOffsetsAndOnlyTheRemainingBytesOfTheLastSample) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1073742000;
  ASSERT_FALSE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  const std::vector<size_t> expectedOffsets = {0,       1024,    4096,     16384,    65536,     262144,
                                               1048576, 4194304, 16777216, 67108864, 268435456, 1073741824};
  EXPECT_EQ(fixture.seeks, expectedOffsets);
  ASSERT_EQ(fixture.reads.size(), expectedOffsets.size());
  for (size_t i = 0; i < expectedOffsets.size(); ++i) {
    EXPECT_EQ(fixture.reads[i].first, expectedOffsets[i]);
    EXPECT_EQ(fixture.reads[i].second, std::min<size_t>(1024, fixture.fileSize - expectedOffsets[i]));
  }
  EXPECT_EQ(fixture.hashFeedSizes.back(), 176U);
}

TEST_F(DocumentId, RejectsOpenFailuresWithoutReadingOrHashing) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1024;
  fixture.openSucceeds = false;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/missing.epub").empty());
  EXPECT_TRUE(fixture.reads.empty());
  EXPECT_TRUE(fixture.hashFeedSizes.empty());
  EXPECT_EQ(fixture.closedFiles, 0);
}

TEST_F(DocumentId, RejectsNegativeReadsBeforeTheyBecomeUnsignedHashLengths) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1024;
  fixture.faultReadOffset = 0;
  fixture.faultReadResult = -1;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  EXPECT_FALSE(fixture.oversizedHashFeed);
  EXPECT_TRUE(fixture.hashFeedSizes.empty());
  EXPECT_EQ(fixture.closedFiles, 1);
}

TEST_F(DocumentId, RejectsZeroByteReadsOfNonemptySamples) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1024;
  fixture.faultReadOffset = 0;
  fixture.faultReadResult = 0;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  EXPECT_TRUE(fixture.hashFeedSizes.empty());
  EXPECT_EQ(fixture.closedFiles, 1);
}

TEST_F(DocumentId, RejectsShortReadsInsteadOfReturningAnIncompleteDocumentId) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1024;
  fixture.faultReadOffset = 0;
  fixture.faultReadResult = 17;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  EXPECT_TRUE(fixture.hashFeedSizes.empty());
  EXPECT_EQ(fixture.closedFiles, 1);
}

TEST_F(DocumentId, RejectsResultsLargerThanTheRequestedSample) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 1024;
  fixture.faultReadOffset = 0;
  fixture.faultReadResult = 1025;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  EXPECT_FALSE(fixture.oversizedHashFeed);
  EXPECT_TRUE(fixture.hashFeedSizes.empty());
}

TEST_F(DocumentId, DiscardsEarlierSamplesIfALaterReadFails) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 4097;
  fixture.faultReadOffset = 1024;
  fixture.faultReadResult = -1;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  EXPECT_FALSE(fixture.oversizedHashFeed);
  EXPECT_EQ(fixture.reads.size(), 2U);
  EXPECT_EQ(fixture.hashFeedSizes, std::vector<size_t>{1024});
  EXPECT_EQ(fixture.closedFiles, 1);
}

TEST_F(DocumentId, RejectsFailedSeeksInsteadOfOmittingASample) {
  auto& fixture = documentIdFixture::state;
  fixture.fileSize = 4097;
  fixture.failedSeek = 1024;
  EXPECT_TRUE(KOReaderDocumentId::calculate("/fixture/book.epub").empty());
  EXPECT_EQ(fixture.seeks, (std::vector<size_t>{0, 1024}));
  EXPECT_EQ(fixture.reads.size(), 1U);
  EXPECT_EQ(fixture.hashFeedSizes, std::vector<size_t>{1024});
  EXPECT_EQ(fixture.closedFiles, 1);
}

TEST_F(DocumentId, FilenameHashingStillIgnoresTheParentDirectory) {
  EXPECT_EQ(KOReaderDocumentId::calculateFromFilename("/one/book.epub"), "03053ffc045564439ff7f2cabb3b58c5");
  EXPECT_EQ(KOReaderDocumentId::calculateFromFilename("/one/book.epub"),
            KOReaderDocumentId::calculateFromFilename("/another/book.epub"));
  EXPECT_TRUE(KOReaderDocumentId::calculateFromFilename("/directory/").empty());
  EXPECT_TRUE(KOReaderDocumentId::calculateFromFilename("").empty());
}

}  // namespace

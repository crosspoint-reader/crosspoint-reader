#include <gtest/gtest.h>

#include <HalStorage.h>
#include <Xtc/XtcPageLayout.h>
#include <Xtc/XtcParser.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr char BOOK_PATH[] = "/book.xtc";

void writeU16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

void writeU64(std::vector<uint8_t>& bytes, const size_t offset, const uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

std::vector<uint8_t> makeBook(const uint8_t bitDepth = 1, const uint16_t width = 480,
                              const uint16_t height = 800, const bool chapters = false) {
  xtc::PageLayout layout;
  EXPECT_TRUE(xtc::calculatePageLayout(width, height, bitDepth, layout));
  const size_t chapterBytes = chapters ? xtc::XTC_CHAPTER_SIZE : 0;
  const size_t metadataOffset = xtc::XTC_HEADER_SIZE;
  const size_t chapterOffset = metadataOffset + xtc::XTC_METADATA_SIZE;
  const size_t tableOffset = chapterOffset + chapterBytes;
  const size_t pageOffset = tableOffset + sizeof(xtc::PageTableEntry);
  const size_t pageBytes = sizeof(xtc::XtgPageHeader) + layout.payloadBytes;
  std::vector<uint8_t> bytes(pageOffset + pageBytes, 0);

  writeU32(bytes, 0, bitDepth == 2 ? xtc::XTCH_MAGIC : xtc::XTC_MAGIC);
  bytes[4] = 1;
  bytes[5] = 0;
  writeU16(bytes, 6, 1);
  bytes[9] = 1;
  bytes[11] = chapters ? 1 : 0;
  writeU32(bytes, 12, 1);
  writeU64(bytes, 16, metadataOffset);
  writeU64(bytes, 24, tableOffset);
  writeU64(bytes, 32, pageOffset);
  writeU64(bytes, 48, chapters ? chapterOffset : 0);

  std::memcpy(bytes.data() + metadataOffset, "CrossVi fixture", 16);
  std::memcpy(bytes.data() + metadataOffset + 128, "CrossVi tests", 13);
  if (chapters) {
    writeU16(bytes, metadataOffset + 196, 1);
    std::memcpy(bytes.data() + chapterOffset, "Chapter 1", 10);
    writeU16(bytes, chapterOffset + 0x50, 1);
    writeU16(bytes, chapterOffset + 0x52, 1);
  }

  writeU64(bytes, tableOffset, pageOffset);
  writeU32(bytes, tableOffset + 8, static_cast<uint32_t>(pageBytes));
  writeU16(bytes, tableOffset + 12, width);
  writeU16(bytes, tableOffset + 14, height);

  writeU32(bytes, pageOffset, bitDepth == 2 ? xtc::XTH_MAGIC : xtc::XTG_MAGIC);
  writeU16(bytes, pageOffset + 4, width);
  writeU16(bytes, pageOffset + 6, height);
  writeU32(bytes, pageOffset + 10, static_cast<uint32_t>(layout.payloadBytes));
  std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(pageOffset + sizeof(xtc::XtgPageHeader)), bytes.end(),
            bitDepth == 1 ? 0xFFU : 0x00U);
  return bytes;
}

uint64_t tableOffset(const std::vector<uint8_t>& bytes) {
  uint64_t value = 0;
  std::memcpy(&value, bytes.data() + 24, sizeof(value));
  return value;
}

uint64_t pageOffset(const std::vector<uint8_t>& bytes) {
  uint64_t value = 0;
  std::memcpy(&value, bytes.data() + 32, sizeof(value));
  return value;
}

xtc::XtcError openBook(std::vector<uint8_t> bytes, xtc::XtcParser* parser = nullptr) {
  Storage.setFile(BOOK_PATH, std::move(bytes));
  xtc::XtcParser local;
  return (parser ? parser : &local)->open(BOOK_PATH);
}

std::vector<uint8_t> readFixture(const char* name) {
  std::ifstream file(std::string(XTC_CONVERTER_FIXTURE_DIR) + "/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

class XtcContractTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(XtcContractTest, CheckedSizesMatchRowAndColumnStorage) {
  xtc::PageLayout xtg;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 800, 1, xtg));
  EXPECT_EQ(xtg.rowBytes, 60U);
  EXPECT_EQ(xtg.payloadBytes, 48000U);

  xtc::PageLayout xth;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 799, 2, xth));
  EXPECT_EQ(xth.columnBytes, 100U);
  EXPECT_EQ(xth.planeBytes, 48000U);
  EXPECT_EQ(xth.payloadBytes, 96000U);
  EXPECT_FALSE(xtc::calculatePageLayout(0, 800, 1, xtg));
  EXPECT_FALSE(xtc::calculatePageLayout(480, 0, 2, xth));
  size_t ignored = 0;
  EXPECT_FALSE(xtc::checkedMultiply(std::numeric_limits<size_t>::max(), 2, ignored));
}

TEST_F(XtcContractTest, OpensSupportedXtcAndXtch) {
  EXPECT_EQ(openBook(makeBook(1)), xtc::XtcError::OK);
  EXPECT_EQ(openBook(makeBook(2)), xtc::XtcError::OK);
}

TEST_F(XtcContractTest, RejectsUnsupportedDimensionsAndVersion) {
  EXPECT_EQ(openBook(makeBook(1, 480, 799)), xtc::XtcError::UNSUPPORTED_DIMENSIONS);
  auto zeroWidth = makeBook();
  writeU16(zeroWidth, tableOffset(zeroWidth) + 12, 0);
  EXPECT_EQ(openBook(std::move(zeroWidth)), xtc::XtcError::UNSUPPORTED_DIMENSIONS);
  auto swappedVersion = makeBook();
  swappedVersion[4] = 0;
  swappedVersion[5] = 1;
  EXPECT_EQ(openBook(std::move(swappedVersion)), xtc::XtcError::INVALID_VERSION);

  auto extremeHeaderDimensions = makeBook();
  writeU16(extremeHeaderDimensions, pageOffset(extremeHeaderDimensions) + 4,
           std::numeric_limits<uint16_t>::max());
  EXPECT_EQ(openBook(std::move(extremeHeaderDimensions)), xtc::XtcError::SIZE_MISMATCH);
}

TEST_F(XtcContractTest, RejectsInvalidHeaderCountsFlagsAndTableRange) {
  auto zeroPages = makeBook();
  writeU16(zeroPages, 6, 0);
  EXPECT_EQ(openBook(std::move(zeroPages)), xtc::XtcError::CORRUPTED_HEADER);

  auto impossibleCurrentPage = makeBook();
  writeU32(impossibleCurrentPage, 12, 2);
  EXPECT_EQ(openBook(std::move(impossibleCurrentPage)), xtc::XtcError::CORRUPTED_HEADER);

  auto invalidFlag = makeBook();
  invalidFlag[9] = 2;
  EXPECT_EQ(openBook(std::move(invalidFlag)), xtc::XtcError::CORRUPTED_HEADER);

  auto missingTableTail = makeBook();
  writeU16(missingTableTail, 6, 2);
  writeU64(missingTableTail, 24, missingTableTail.size() - sizeof(xtc::PageTableEntry));
  EXPECT_EQ(openBook(std::move(missingTableTail)), xtc::XtcError::OFFSET_OUT_OF_RANGE);

  auto wrappedTable = makeBook();
  writeU64(wrappedTable, 24, std::numeric_limits<uint64_t>::max() - 4U);
  EXPECT_EQ(openBook(std::move(wrappedTable)), xtc::XtcError::OFFSET_OUT_OF_RANGE);
}

TEST_F(XtcContractTest, RejectsWrongContainerAndPageMagic) {
  auto wrongContainer = makeBook();
  writeU32(wrongContainer, 0, 0x12345678U);
  EXPECT_EQ(openBook(std::move(wrongContainer)), xtc::XtcError::INVALID_MAGIC);

  auto mismatchedPage = makeBook(1);
  writeU32(mismatchedPage, pageOffset(mismatchedPage), xtc::XTH_MAGIC);
  EXPECT_EQ(openBook(std::move(mismatchedPage)), xtc::XtcError::INVALID_MAGIC);
  auto mismatchedGrayPage = makeBook(2);
  writeU32(mismatchedGrayPage, pageOffset(mismatchedGrayPage), xtc::XTG_MAGIC);
  EXPECT_EQ(openBook(std::move(mismatchedGrayPage)), xtc::XtcError::INVALID_MAGIC);
}

TEST_F(XtcContractTest, RejectsSizeOffsetCompressionAndTruncationDamage) {
  auto tableMismatch = makeBook();
  writeU32(tableMismatch, tableOffset(tableMismatch) + 8, 1234);
  EXPECT_EQ(openBook(std::move(tableMismatch)), xtc::XtcError::SIZE_MISMATCH);

  auto headerMismatch = makeBook();
  writeU32(headerMismatch, pageOffset(headerMismatch) + 10, 1234);
  EXPECT_EQ(openBook(std::move(headerMismatch)), xtc::XtcError::SIZE_MISMATCH);

  auto compressed = makeBook();
  compressed[pageOffset(compressed) + 9] = 1;
  EXPECT_EQ(openBook(std::move(compressed)), xtc::XtcError::UNSUPPORTED_COMPRESSION);

  auto outside = makeBook();
  writeU64(outside, tableOffset(outside), std::numeric_limits<uint64_t>::max() - 4U);
  EXPECT_EQ(openBook(std::move(outside)), xtc::XtcError::OFFSET_OUT_OF_RANGE);

  auto truncated = makeBook();
  truncated.pop_back();
  EXPECT_EQ(openBook(std::move(truncated)), xtc::XtcError::OFFSET_OUT_OF_RANGE);
}

TEST_F(XtcContractTest, RejectsMalformedMetadataAndChapters) {
  auto noTitleTerminator = makeBook();
  std::fill(noTitleTerminator.begin() + xtc::XTC_HEADER_SIZE,
            noTitleTerminator.begin() + xtc::XTC_HEADER_SIZE + 128, 'A');
  EXPECT_EQ(openBook(std::move(noTitleTerminator)), xtc::XtcError::INVALID_METADATA);

  auto noAuthorTerminator = makeBook();
  std::fill(noAuthorTerminator.begin() + xtc::XTC_HEADER_SIZE + 128,
            noAuthorTerminator.begin() + xtc::XTC_HEADER_SIZE + 192, 'A');
  EXPECT_EQ(openBook(std::move(noAuthorTerminator)), xtc::XtcError::INVALID_METADATA);

  auto truncatedMetadata = makeBook();
  truncatedMetadata.resize(xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE - 1U);
  writeU64(truncatedMetadata, 24, truncatedMetadata.size());
  writeU64(truncatedMetadata, 32, truncatedMetadata.size());
  EXPECT_EQ(openBook(std::move(truncatedMetadata)), xtc::XtcError::INVALID_METADATA);

  auto badChapterOffset = makeBook(1, 480, 800, true);
  writeU64(badChapterOffset, 48, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(openBook(std::move(badChapterOffset)), xtc::XtcError::INVALID_CHAPTERS);

  auto tooManyChapters = makeBook(1, 480, 800, true);
  writeU16(tooManyChapters, xtc::XTC_HEADER_SIZE + 196, xtc::XTC_MAX_CHAPTERS + 1U);
  EXPECT_EQ(openBook(std::move(tooManyChapters)), xtc::XtcError::INVALID_CHAPTERS);

  auto pageOutside = makeBook(1, 480, 800, true);
  writeU16(pageOutside, xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 0x50, 2);
  writeU16(pageOutside, xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 0x52, 2);
  EXPECT_EQ(openBook(std::move(pageOutside)), xtc::XtcError::INVALID_CHAPTERS);

  auto invertedRange = makeBook(1, 480, 800, true);
  writeU16(invertedRange, xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 0x50, 1);
  writeU16(invertedRange, xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 0x52, 0);
  EXPECT_EQ(openBook(std::move(invertedRange)), xtc::XtcError::INVALID_CHAPTERS);

  auto unterminatedChapter = makeBook(1, 480, 800, true);
  std::fill(unterminatedChapter.begin() + xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE,
            unterminatedChapter.begin() + xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 80, 'C');
  EXPECT_EQ(openBook(std::move(unterminatedChapter)), xtc::XtcError::INVALID_CHAPTERS);

  auto chapterPointsIntoPayload = makeBook(1, 480, 800, true);
  writeU64(chapterPointsIntoPayload, 48, pageOffset(chapterPointsIntoPayload) + sizeof(xtc::XtgPageHeader));
  EXPECT_EQ(openBook(std::move(chapterPointsIntoPayload)), xtc::XtcError::INVALID_CHAPTERS);
}

TEST_F(XtcContractTest, FailedOpenClearsPreviouslyLoadedState) {
  xtc::XtcParser parser;
  ASSERT_EQ(openBook(makeBook(1, 480, 800, true), &parser), xtc::XtcError::OK);
  ASSERT_TRUE(parser.isOpen());
  ASSERT_TRUE(parser.hasChapters());
  ASSERT_FALSE(parser.getTitle().empty());

  auto broken = makeBook();
  writeU32(broken, 0, 0);
  EXPECT_EQ(openBook(std::move(broken), &parser), xtc::XtcError::INVALID_MAGIC);
  EXPECT_FALSE(parser.isOpen());
  EXPECT_FALSE(parser.hasChapters());
  EXPECT_TRUE(parser.getTitle().empty());
  ZipFile::SourceIdentity identity;
  EXPECT_FALSE(parser.getSourceIdentity(identity));
}

TEST_F(XtcContractTest, PageLoadingRequiresExactBoundsAndKeepsStreamingChunksBounded) {
  xtc::XtcParser parser;
  ASSERT_EQ(openBook(makeBook(), &parser), xtc::XtcError::OK);
  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 800, 1, layout));

  std::vector<uint8_t> tooSmall(layout.payloadBytes - 1U);
  EXPECT_EQ(parser.loadPage(0, tooSmall.data(), tooSmall.size()), 0U);
  EXPECT_EQ(parser.getLastError(), xtc::XtcError::MEMORY_ERROR);

  std::vector<uint8_t> exact(layout.payloadBytes);
  EXPECT_EQ(parser.loadPage(0, exact.data(), exact.size()), layout.payloadBytes);
  EXPECT_TRUE(std::all_of(exact.begin(), exact.end(), [](const uint8_t value) { return value == 0xFFU; }));

  size_t streamed = 0;
  size_t largestChunk = 0;
  EXPECT_EQ(parser.loadPageStreaming(
                0,
                [&](const uint8_t*, const size_t size, const size_t offset) {
                  EXPECT_EQ(offset, streamed);
                  streamed += size;
                  largestChunk = std::max(largestChunk, size);
                },
                std::numeric_limits<size_t>::max()),
            xtc::XtcError::OK);
  EXPECT_EQ(streamed, layout.payloadBytes);
  EXPECT_LE(largestChunk, 1024U);
  EXPECT_EQ(parser.loadPageStreaming(0, {}, 1024), xtc::XtcError::INVALID_ARGUMENT);
  EXPECT_EQ(parser.loadPageStreaming(1, [](const uint8_t*, size_t, size_t) {}, 1024),
            xtc::XtcError::PAGE_OUT_OF_RANGE);
}

TEST_F(XtcContractTest, ReadFailuresAndSourceMutationFailClosed) {
  Storage.setFile(BOOK_PATH, makeBook());
  Storage.shortReadFor(BOOK_PATH);
  xtc::XtcParser shortReadParser;
  EXPECT_EQ(shortReadParser.open(BOOK_PATH), xtc::XtcError::READ_ERROR);
  EXPECT_FALSE(shortReadParser.isOpen());

  Storage.reset();
  Storage.setFile(BOOK_PATH, makeBook());
  // Header, metadata, page-table entry and page header are the first four
  // reads; mutate the file as the streaming identity pass begins.
  Storage.growOnReadCall(5);
  xtc::XtcParser changingParser;
  EXPECT_EQ(changingParser.open(BOOK_PATH), xtc::XtcError::READ_ERROR);
  EXPECT_FALSE(changingParser.isOpen());
}

TEST_F(XtcContractTest, XthPlaneOrderPreservesAllFourConverterLevels) {
  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 800, 2, layout));
  std::vector<uint8_t> payload(layout.payloadBytes, 0);
  const auto setLevel = [&](const uint16_t x, const uint16_t y, const uint8_t level) {
    const size_t offset = static_cast<size_t>(480 - 1 - x) * layout.columnBytes + y / 8U;
    const uint8_t bit = static_cast<uint8_t>(7U - y % 8U);
    if ((level & 1U) != 0) payload[offset] |= static_cast<uint8_t>(1U << bit);
    if ((level & 2U) != 0) payload[layout.planeBytes + offset] |= static_cast<uint8_t>(1U << bit);
  };
  for (uint8_t level = 0; level < 4; ++level) setLevel(level, 0, level);
  for (uint8_t level = 0; level < 4; ++level) {
    EXPECT_EQ(xtc::readXthPixel(payload.data(), layout, 480, level, 0), level);
  }
}

TEST_F(XtcContractTest, X3FitsAndX4MapsEveryEdge) {
  xtc::Viewport x4;
  ASSERT_TRUE(xtc::calculateFitViewport(480, 800, 480, 800, x4));
  EXPECT_EQ(x4.x, 0);
  EXPECT_EQ(x4.y, 0);
  EXPECT_EQ(x4.width, 480);
  EXPECT_EQ(x4.height, 800);

  xtc::Viewport x3;
  ASSERT_TRUE(xtc::calculateFitViewport(480, 800, 528, 792, x3));
  EXPECT_EQ(x3.x, 26);
  EXPECT_EQ(x3.y, 0);
  EXPECT_EQ(x3.width, 475);
  EXPECT_EQ(x3.height, 792);
  EXPECT_EQ(xtc::mapViewportCoordinate(0, x3.width, 480), 0);
  EXPECT_EQ(xtc::mapViewportCoordinate(x3.width - 1, x3.width, 480), 479);
  EXPECT_EQ(xtc::mapViewportCoordinate(0, x3.height, 800), 0);
  EXPECT_EQ(xtc::mapViewportCoordinate(x3.height - 1, x3.height, 800), 799);
}

TEST_F(XtcContractTest, SameSizePixelReplacementChangesStreamingIdentity) {
  auto first = makeBook();
  auto second = first;
  second.back() ^= 0x01U;
  xtc::XtcParser firstParser;
  ASSERT_EQ(openBook(std::move(first), &firstParser), xtc::XtcError::OK);
  ZipFile::SourceIdentity firstIdentity;
  ASSERT_TRUE(firstParser.getSourceIdentity(firstIdentity));
  xtc::XtcParser secondParser;
  ASSERT_EQ(openBook(std::move(second), &secondParser), xtc::XtcError::OK);
  ZipFile::SourceIdentity secondIdentity;
  ASSERT_TRUE(secondParser.getSourceIdentity(secondIdentity));
  EXPECT_EQ(firstIdentity.fileSize, secondIdentity.fileSize);
  EXPECT_NE(firstIdentity, secondIdentity);
  EXPECT_LE(Storage.maxRead(), 2048U);
}

TEST_F(XtcContractTest, OpensFrozenFixturesFromTheRecommendedConverter) {
  const auto xtcFixture = readFixture("crossvi-converter-480x800.xtc");
  const auto xtchFixture = readFixture("crossvi-converter-480x800.xtch");
  ASSERT_FALSE(xtcFixture.empty());
  ASSERT_FALSE(xtchFixture.empty());
  EXPECT_EQ(openBook(xtcFixture), xtc::XtcError::OK);
  EXPECT_EQ(openBook(xtchFixture), xtc::XtcError::OK);
}

}  // namespace

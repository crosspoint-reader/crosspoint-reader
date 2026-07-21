#include <gtest/gtest.h>

#include "Epub/Epub/SectionCacheLayout.h"

namespace {
constexpr uint64_t HEADER_SIZE = 37;

SectionCacheLayout::FinalizedHeader validHeader() {
  // Two serialized pages occupy bytes [37, 137), followed by the four
  // finalized lookup tables written by Section::commitBuildFile().
  return {2, 137, 145, 151, 157};
}
}  // namespace

TEST(SectionCacheLayout, AcceptsExactFinalizedTableBoundaries) {
  EXPECT_FALSE(SectionCacheLayout::hasCompleteHeader(HEADER_SIZE - 1, HEADER_SIZE));
  EXPECT_TRUE(SectionCacheLayout::hasCompleteHeader(HEADER_SIZE, HEADER_SIZE));
  EXPECT_TRUE(SectionCacheLayout::isValidFinalized(161, HEADER_SIZE, validHeader(), 2));
}

TEST(SectionCacheLayout, RejectsHeaderOnlyAndMalformedFinalizedFiles) {
  auto header = validHeader();
  header.pageLutOffset = 0;
  EXPECT_FALSE(SectionCacheLayout::isValidFinalized(161, HEADER_SIZE, header, 2));

  header = validHeader();
  header.anchorMapOffset++;
  EXPECT_FALSE(SectionCacheLayout::isValidFinalized(161, HEADER_SIZE, header, 2));

  header = validHeader();
  header.paragraphLutOffset--;
  EXPECT_FALSE(SectionCacheLayout::isValidFinalized(161, HEADER_SIZE, header, 2));

  header = validHeader();
  EXPECT_FALSE(SectionCacheLayout::isValidFinalized(161, HEADER_SIZE, header, 1));
  EXPECT_FALSE(SectionCacheLayout::isValidFinalized(160, HEADER_SIZE, header, 2));

  header = validHeader();
  header.pageCount = 0;
  EXPECT_FALSE(SectionCacheLayout::isValidFinalized(161, HEADER_SIZE, header, 0));
}

TEST(SectionCacheLayout, ChecksPartialTrailerWithWideArithmetic) {
  constexpr uint32_t LARGE_OFFSET = UINT32_MAX;
  constexpr uint16_t LARGE_PAGE_COUNT = UINT16_MAX;
  const uint64_t trailerOffset =
      static_cast<uint64_t>(LARGE_OFFSET) + static_cast<uint64_t>(LARGE_PAGE_COUNT) * sizeof(uint16_t);

  EXPECT_EQ(SectionCacheLayout::partialTrailerOffset(LARGE_OFFSET, LARGE_PAGE_COUNT), trailerOffset);
  EXPECT_FALSE(SectionCacheLayout::hasCompletePartialTrailer(UINT32_MAX, HEADER_SIZE, LARGE_OFFSET, LARGE_PAGE_COUNT));
  EXPECT_FALSE(
      SectionCacheLayout::hasCompletePartialTrailer(trailerOffset + 7, HEADER_SIZE, LARGE_OFFSET, LARGE_PAGE_COUNT));
  EXPECT_TRUE(
      SectionCacheLayout::hasCompletePartialTrailer(trailerOffset + 8, HEADER_SIZE, LARGE_OFFSET, LARGE_PAGE_COUNT));
}

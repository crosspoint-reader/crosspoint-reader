#include <gtest/gtest.h>

#include <limits>

#include "ProgressComparison.h"

namespace {
CrossPointPosition mappedPage(const int spine, const int page) {
  CrossPointPosition position{};
  position.spineIndex = spine;
  position.pageNumber = page;
  position.totalPages = 20;
  position.hasResolvedSpineIndex = true;
  position.hasMappedPage = true;
  return position;
}

CrossPointPosition resolvedOffset(const int spine, const int page, const uint32_t offset) {
  CrossPointPosition position = mappedPage(spine, page);
  position.visibleTextOffset = offset;
  position.hasVisibleTextOffset = true;
  return position;
}
}  // namespace

TEST(ProgressComparison, LaterLocalSpineWinsDespiteLowerPercentage) {
  EXPECT_EQ(compareProgress(mappedPage(4, 1), 0.10f, mappedPage(3, 18), 0.90f), ProgressComparison::LocalAhead);
}

TEST(ProgressComparison, LaterRemoteSpineWinsDespiteLowerPercentage) {
  EXPECT_EQ(compareProgress(mappedPage(3, 18), 0.90f, mappedPage(4, 1), 0.10f), ProgressComparison::RemoteAhead);
}

TEST(ProgressComparison, Issue2982RemoteLaterPageWinsDespiteLowerPercentage) {
  EXPECT_EQ(compareProgress(mappedPage(3, 4), 0.1715f, mappedPage(3, 15), 0.1477f), ProgressComparison::RemoteAhead);
}

TEST(ProgressComparison, Issue2982LocalLaterPageWinsDespiteLowerPercentage) {
  EXPECT_EQ(compareProgress(mappedPage(3, 15), 0.1477f, mappedPage(3, 4), 0.1715f), ProgressComparison::LocalAhead);
}

TEST(ProgressComparison, MatchingMappedPageIsSynchronizedBeforeOffsetComparison) {
  EXPECT_EQ(compareProgress(resolvedOffset(2, 7, 900), 0.4f, resolvedOffset(2, 7, 1200), 0.5f),
            ProgressComparison::Synchronized);
}

TEST(ProgressComparison, VisibleOffsetsOrderDifferentMappedPages) {
  EXPECT_EQ(compareProgress(resolvedOffset(2, 7, 1500), 0.4f, resolvedOffset(2, 8, 1200), 0.5f),
            ProgressComparison::LocalAhead);
}

TEST(ProgressComparison, MappedPagesOrderPositionsWithoutOffsets) {
  EXPECT_EQ(compareProgress(mappedPage(2, 7), 0.6f, mappedPage(2, 8), 0.5f), ProgressComparison::RemoteAhead);
}

TEST(ProgressComparison, PercentageFallbackRetainsTolerance) {
  const CrossPointPosition estimated{};
  EXPECT_EQ(compareProgress(estimated, 0.5000f, estimated, 0.5009f), ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(estimated, 0.502f, estimated, 0.500f), ProgressComparison::LocalAhead);
  EXPECT_EQ(compareProgress(estimated, 0.500f, estimated, 0.502f), ProgressComparison::RemoteAhead);
}

TEST(ProgressComparison, InvalidPercentageWithoutMappedEvidenceIsUnknown) {
  const CrossPointPosition estimated{};
  EXPECT_EQ(compareProgress(estimated, std::numeric_limits<float>::quiet_NaN(), estimated, 0.5f),
            ProgressComparison::Unknown);
  EXPECT_EQ(compareProgress(estimated, 0.5f, estimated, std::numeric_limits<float>::infinity()),
            ProgressComparison::Unknown);
}

TEST(ProgressComparison, RichPageOnlyPositionsUseTrustedPages) {
  EXPECT_EQ(compareProgress(mappedPage(5, 9), 0.8f, mappedPage(5, 12), 0.2f), ProgressComparison::RemoteAhead);
  EXPECT_EQ(compareProgress(mappedPage(5, 9), 0.8f, mappedPage(5, 9), 0.2f), ProgressComparison::Synchronized);
}

TEST(ProgressComparison, AlternateRecordMustBeStrictlyAhead) {
  EXPECT_EQ(selectRemoteRecord(mappedPage(2, 4), 0.8f, mappedPage(3, 1), 0.2f), RemoteRecordChoice::Alternate);
  EXPECT_EQ(selectRemoteRecord(mappedPage(2, 4), 0.8f, mappedPage(2, 4), 0.2f), RemoteRecordChoice::Primary);

  const CrossPointPosition estimated{};
  EXPECT_EQ(selectRemoteRecord(estimated, std::numeric_limits<float>::quiet_NaN(), estimated,
                               std::numeric_limits<float>::infinity()),
            RemoteRecordChoice::Primary);
}

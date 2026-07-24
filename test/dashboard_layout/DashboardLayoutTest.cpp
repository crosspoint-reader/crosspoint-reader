#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>

#include "activities/home/DashboardProgress.h"
#include "activities/home/DashboardStatsPolicy.h"
#include "activities/home/HomeBookSummary.h"
#include "components/themes/HomeMenuLayout.h"
#include "components/themes/crossvi/CrossViLayout.h"
#include "components/themes/crossvi/CrossViTheme.h"
#include "components/themes/dashboard/DashboardLayout.h"

namespace {

bool contains(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.width >= 0 && inner.height >= 0 &&
         inner.x + inner.width <= outer.x + outer.width && inner.y + inner.height <= outer.y + outer.height;
}

bool overlaps(const Rect& a, const Rect& b) {
  return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width && a.x + a.width > b.x &&
         a.y < b.y + b.height && a.y + a.height > b.y;
}

}  // namespace

TEST(HomeMenuLayout, PreservesPreferredRhythmWhenItFits) {
  const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(430, 6, 1, 64, 8, 40);
  EXPECT_EQ(layout.rows, 6);
  EXPECT_EQ(layout.rowHeight, 64);
  EXPECT_EQ(layout.rowGap, 8);
  EXPECT_EQ(layout.totalHeight(), 424);
}

TEST(HomeMenuLayout, CompressesEveryRowInsideTheAvailableHeight) {
  for (const int height : {292, 317, 364, 422}) {
    for (const int itemCount : {6, 7, 8}) {
      const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(height, itemCount, 1, 64, 8, 32);
      EXPECT_GT(layout.rowHeight, 0);
      EXPECT_LE(layout.totalHeight(), height);
      EXPECT_EQ(layout.yOffset(itemCount - 1, 1) + layout.rowHeight, layout.totalHeight());
    }
  }
}

TEST(HomeMenuLayout, FitsTwoColumnDashboardMenus) {
  const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(262, 7, 2, 48, 8, 30);
  EXPECT_EQ(layout.rows, 4);
  EXPECT_EQ(layout.rowHeight, 48);
  EXPECT_EQ(layout.rowGap, 8);
  EXPECT_LE(layout.totalHeight(), 262);
  EXPECT_EQ(layout.yOffset(6, 2), 3 * (layout.rowHeight + layout.rowGap));
}

TEST(DashboardLayout, FitsBothSupportedPortraitPanels) {
  for (const Rect tile : std::array<Rect, 2>{Rect{0, 56, 480, 410}, Rect{0, 56, 528, 410}}) {
    const DashboardLayout layout = DashboardLayout::calculate(tile);

    EXPECT_TRUE(contains(tile, layout.content));
    EXPECT_TRUE(contains(tile, layout.cover));
    EXPECT_TRUE(contains(tile, layout.stats));
    EXPECT_TRUE(contains(tile, layout.title));
    EXPECT_TRUE(contains(tile, layout.progress));
    EXPECT_TRUE(contains(tile, layout.progressBar));
    EXPECT_TRUE(contains(tile, layout.progressLabel));
    EXPECT_TRUE(contains(tile, layout.footer));

    EXPECT_FALSE(overlaps(layout.cover, layout.stats));
    EXPECT_FALSE(overlaps(layout.cover, layout.title));
    EXPECT_FALSE(overlaps(layout.stats, layout.title));
    EXPECT_FALSE(overlaps(layout.title, layout.progress));
    EXPECT_FALSE(overlaps(layout.progressBar, layout.progressLabel));
    EXPECT_FALSE(overlaps(layout.progressLabel, layout.footer));
    EXPECT_FALSE(overlaps(layout.progress, layout.footer));
  }
}

TEST(DashboardLayout, UsesOnlyTheCoverForTheReusableBitmapCache) {
  const DashboardLayout x4 = DashboardLayout::calculate(Rect{0, 56, 480, 410});
  const DashboardLayout x3 = DashboardLayout::calculate(Rect{0, 56, 528, 410});

  EXPECT_EQ(x4.cover.width, 168);
  EXPECT_EQ(x4.cover.height, 252);
  EXPECT_EQ(x3.cover.width, 168);
  EXPECT_EQ(x3.cover.height, 252);

  // One bit per pixel, rounded per scanline by the renderer. This upper bound
  // stays well below CrossInk's 41-46 KB whole-dashboard snapshot.
  EXPECT_LT(((x4.cover.width + 7) / 8) * x4.cover.height, 6000);
  EXPECT_LT(((x3.cover.width + 7) / 8) * x3.cover.height, 6000);
}

TEST(DashboardLayout, DegradesWithoutNegativeOrOutOfBoundsRects) {
  const Rect tinyTile{10, 20, 120, 100};
  const DashboardLayout layout = DashboardLayout::calculate(tinyTile);

  EXPECT_TRUE(contains(tinyTile, layout.content));
  EXPECT_TRUE(contains(tinyTile, layout.cover));
  EXPECT_TRUE(contains(tinyTile, layout.stats));
  EXPECT_TRUE(contains(tinyTile, layout.title));
  EXPECT_TRUE(contains(tinyTile, layout.progress));
  EXPECT_TRUE(contains(tinyTile, layout.progressBar));
  EXPECT_TRUE(contains(tinyTile, layout.progressLabel));
  EXPECT_TRUE(contains(tinyTile, layout.footer));
}

TEST(CrossViLayout, FitsBothSupportedPortraitPanels) {
  for (const Rect tile : std::array<Rect, 2>{Rect{0, 56, 480, 252}, Rect{0, 56, 528, 252}}) {
    const CrossViLayout layout = CrossViLayout::calculate(tile);

    EXPECT_TRUE(contains(tile, layout.card));
    EXPECT_TRUE(contains(layout.card, layout.cover));
    EXPECT_TRUE(contains(layout.card, layout.details));
    EXPECT_TRUE(contains(layout.details, layout.title));
    EXPECT_TRUE(contains(layout.details, layout.stats));
    EXPECT_TRUE(contains(layout.details, layout.progress));
    EXPECT_TRUE(contains(layout.progress, layout.progressBar));
    EXPECT_TRUE(contains(layout.progress, layout.progressLabel));
    EXPECT_TRUE(contains(layout.details, layout.continueButton));
    EXPECT_TRUE(contains(layout.details, layout.continueButtonWithoutProgress));

    EXPECT_FALSE(overlaps(layout.cover, layout.details));
    EXPECT_FALSE(overlaps(layout.title, layout.stats));
    EXPECT_FALSE(overlaps(layout.stats, layout.progress));
    EXPECT_FALSE(overlaps(layout.progress, layout.continueButton));
    EXPECT_FALSE(overlaps(layout.stats, layout.continueButtonWithoutProgress));
  }
}

TEST(CrossViLayout, CachesOnlyTheCompactCoverRegion) {
  const CrossViLayout x4 = CrossViLayout::calculate(Rect{0, 56, 480, 252});
  const CrossViLayout x3 = CrossViLayout::calculate(Rect{0, 56, 528, 252});

  EXPECT_EQ(x4.cover.x - x4.card.x, 20);
  EXPECT_EQ(x3.cover.x - x3.card.x, 20);
  EXPECT_EQ(x4.cover.width, 92);
  EXPECT_EQ(x4.cover.height, 138);
  EXPECT_EQ(x3.cover.width, 100);
  EXPECT_EQ(x3.cover.height, 150);
  EXPECT_LT(((x4.cover.width + 7) / 8) * x4.cover.height, 3000);
  EXPECT_LT(((x3.cover.width + 7) / 8) * x3.cover.height, 4000);
}

TEST(CrossViLayout, AdaptiveTwoColumnMenuKeepsSettingsInsideTheSafeArea) {
  // The two-row reading summary reserves 76 px below the menu.
  for (const int availableHeight : {316, 324}) {
    for (const int itemCount : {6, 7}) {
      const HomeMenuLayout::Fit layout = HomeMenuLayout::fit(availableHeight, itemCount, 2, 80, 10, 48);
      EXPECT_GT(layout.rowHeight, 0);
      EXPECT_LE(layout.totalHeight(), availableHeight);
      EXPECT_LE(layout.yOffset(itemCount - 1, 2) + layout.rowHeight, availableHeight);
    }
  }
}

TEST(CrossViLayout, ReadingSummaryFitsBetweenMenuAndButtonHints) {
  for (const Rect screen : std::array<Rect, 2>{Rect{0, 0, 528, 792}, Rect{0, 0, 480, 800}}) {
    const int screenHeight = screen.height;
    const int contentBottom = screenHeight - CrossViMetrics::values.buttonHintsHeight -
                              CrossViMetrics::values.verticalSpacing;
    const Rect summary{CrossViMetrics::values.contentSidePadding,
                       contentBottom - CrossViMetrics::HOME_READING_SUMMARY_HEIGHT -
                           CrossViMetrics::HOME_READING_SUMMARY_BOTTOM_GAP,
                       screen.width - CrossViMetrics::values.contentSidePadding * 2,
                       CrossViMetrics::HOME_READING_SUMMARY_HEIGHT};
    EXPECT_GT(summary.width, 0);
    EXPECT_EQ(summary.y + summary.height + CrossViMetrics::HOME_READING_SUMMARY_BOTTOM_GAP, contentBottom);
    EXPECT_LE(summary.y + summary.height,
              screenHeight - CrossViMetrics::values.buttonHintsHeight - CrossViMetrics::values.verticalSpacing);
  }
}

TEST(CrossViHomeAction, UsesOnlyTrustedExactProgressForItsLabel) {
  HomeBookSummary summary;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Open);

  summary.bookStatsState = DashboardMetricState::NoData;
  summary.progressState = DashboardMetricState::NoData;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Start);

  summary.hasProgress = true;
  summary.progressState = DashboardMetricState::Available;
  summary.progressPercent = 42;
  summary.hasStartedReading = true;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Continue);

  summary.progressEstimated = true;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Continue);

  summary.progressEstimated = false;
  summary.progressPercent = 100;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::ReadAgain);
}

TEST(CrossViHomeAction, DoesNotTreatOpeningTheFirstSubPercentPageAsReading) {
  HomeBookSummary summary;
  summary.bookStatsState = DashboardMetricState::NoData;
  summary.progressState = DashboardMetricState::Available;
  summary.hasProgress = true;
  summary.progressBelowOnePercent = true;

  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Start);

  summary.hasStartedReading = true;
  EXPECT_EQ(homeBookAction(summary), HomeBookAction::Continue);
}

TEST(DashboardProgress, StrictlyDecodesFinalizedEpubProgress) {
  const std::array<uint8_t, 6> valid{2, 0, 4, 0, 10, 0};
  DashboardProgress::Position position;
  ASSERT_TRUE(DashboardProgress::decode(valid.data(), valid.size(), position));
  EXPECT_EQ(position.spineIndex, 2);
  EXPECT_EQ(position.pageNumber, 4);
  EXPECT_EQ(position.pageCount, 10);

  EXPECT_FALSE(DashboardProgress::decode(valid.data(), 4, position));
  auto invalid = valid;
  invalid[2] = 10;
  EXPECT_FALSE(DashboardProgress::decode(invalid.data(), invalid.size(), position));
  invalid = valid;
  invalid[4] = 0;
  invalid[5] = 0;
  EXPECT_FALSE(DashboardProgress::decode(invalid.data(), invalid.size(), position));
  invalid = valid;
  invalid[2] = 0xFF;
  invalid[3] = 0xFF;
  EXPECT_FALSE(DashboardProgress::decode(invalid.data(), invalid.size(), position));
}

TEST(DashboardProgress, RequiresMatchingFinalizedSectionCache) {
  const DashboardProgress::Position position{2, 4, 10};
  EXPECT_TRUE(DashboardProgress::validate(position, 3, std::optional<uint16_t>{10}));
  EXPECT_FALSE(DashboardProgress::validate(position, 2, std::optional<uint16_t>{10}));
  EXPECT_FALSE(DashboardProgress::validate(position, 3, std::optional<uint16_t>{9}));
  EXPECT_FALSE(DashboardProgress::validate(position, 3, std::nullopt));
}

TEST(DashboardProgress, ConvertsOnlyFiniteProgress) {
  uint8_t percent = 99;
  EXPECT_TRUE(DashboardProgress::toPercent(0.425F, percent));
  EXPECT_EQ(percent, 43);
  EXPECT_TRUE(DashboardProgress::toPercent(-1.0F, percent));
  EXPECT_EQ(percent, 0);
  EXPECT_TRUE(DashboardProgress::toPercent(2.0F, percent));
  EXPECT_EQ(percent, 100);
  EXPECT_FALSE(DashboardProgress::toPercent(std::numeric_limits<float>::quiet_NaN(), percent));
}

TEST(DashboardProgress, TrustedCompletionDoesNotDependOnProgressOrSectionCache) {
  uint8_t percent = 7;
  EXPECT_TRUE(DashboardProgress::fromCompletedStats(true, true, percent));
  EXPECT_EQ(percent, 100);

  percent = 7;
  EXPECT_FALSE(DashboardProgress::fromCompletedStats(false, true, percent));
  EXPECT_EQ(percent, 7);
  EXPECT_FALSE(DashboardProgress::fromCompletedStats(true, false, percent));
  EXPECT_EQ(percent, 7);
}

TEST(DashboardProgress, CompactBarFillIsExactAndBounded) {
  EXPECT_EQ(DashboardProgress::fillWidth(200, 0), 0);
  EXPECT_EQ(DashboardProgress::fillWidth(200, 100), 196);
  EXPECT_EQ(DashboardProgress::fillWidth(200, 255), 196);
  EXPECT_EQ(DashboardProgress::fillWidth(4, 100), 0);
}

TEST(DashboardStatsPolicy, DistinguishesMissingZeroAndUnreadableBookStats) {
  DashboardStatsPolicyInput input;
  input.isEpub = true;
  input.epubVerified = true;
  input.localStatsTrusted = true;
  input.localStatsMissing = true;

  input.bookStatsTrusted = true;
  input.bookStatsMissing = true;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::NoData);

  input.bookStatsMissing = false;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::Available);

  input.bookStatsTrusted = false;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::Unavailable);
}

TEST(DashboardStatsPolicy, NonEpubFormatsAreExplicitlyNotTracked) {
  DashboardStatsPolicyInput input;
  input.localStatsTrusted = true;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::NotTracked);

  input.isEpub = true;
  EXPECT_EQ(DashboardStatsPolicy::evaluate(input).bookStats, DashboardMetricState::Unavailable);
}

TEST(DashboardStatsPolicy, UsesOnlyACompleteVerifiedSyncedAggregate) {
  DashboardStatsPolicyInput input;
  input.localStatsTrusted = true;
  input.hasSyncedDirectory = true;
  input.syncedScanComplete = true;
  input.validPeerCount = 2;

  DashboardStatsPolicyResult result = DashboardStatsPolicy::evaluate(input);
  EXPECT_TRUE(result.useAllSynced);
  EXPECT_EQ(result.globalStats, DashboardMetricState::Available);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::Available);

  input.skippedPeerCount = 1;
  result = DashboardStatsPolicy::evaluate(input);
  EXPECT_FALSE(result.useAllSynced);
  EXPECT_EQ(result.globalStats, DashboardMetricState::Available);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::Unavailable);

  input.skippedPeerCount = 0;
  input.syncedScanComplete = false;
  result = DashboardStatsPolicy::evaluate(input);
  EXPECT_FALSE(result.useAllSynced);
  EXPECT_EQ(result.globalStats, DashboardMetricState::Available);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::Unavailable);
}

TEST(DashboardStatsPolicy, UntrustedLocalStatsCannotBecomePlausibleSyncedZeros) {
  DashboardStatsPolicyInput input;
  input.hasSyncedDirectory = true;
  input.syncedScanComplete = true;
  input.validPeerCount = 2;

  const DashboardStatsPolicyResult result = DashboardStatsPolicy::evaluate(input);
  EXPECT_FALSE(result.useAllSynced);
  EXPECT_EQ(result.globalStats, DashboardMetricState::Unavailable);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::Unavailable);
}

TEST(DashboardStatsPolicy, MissingLocalStatsAreKnownNoDataWithoutPeers) {
  DashboardStatsPolicyInput input;
  input.localStatsTrusted = true;
  input.localStatsMissing = true;

  const DashboardStatsPolicyResult result = DashboardStatsPolicy::evaluate(input);
  EXPECT_EQ(result.globalStats, DashboardMetricState::NoData);
  EXPECT_EQ(result.syncedStats, DashboardMetricState::NoData);
}

#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "ClippingPageTools.h"

namespace {

Page makePage(const int focusSuffix = 0, const int leftMargin = 0, const bool withImage = false) {
  BlockStyle style;
  style.marginLeft = static_cast<int16_t>(leftMargin);
  style.textAlignDefined = true;
  auto block =
      std::make_shared<TextBlock>(std::vector<std::string>{"Xin", "chào"}, std::vector<int16_t>{0, 30},
                                  std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR, EpdFontFamily::BOLD},
                                  std::vector<uint8_t>{0, static_cast<uint8_t>(focusSuffix == 0 ? 0 : 2)},
                                  std::vector<uint16_t>{0, static_cast<uint16_t>(focusSuffix)}, style);

  Page page;
  page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, 20));
  if (withImage) page.elements.push_back(std::make_shared<PageImage>(80, 60, 12, 90));
  return page;
}

Page makeWordPage(const size_t count) {
  std::vector<std::string> words(count, "x");
  std::vector<int16_t> positions(count, 0);
  std::vector<EpdFontFamily::Style> styles(count, EpdFontFamily::REGULAR);
  auto block = std::make_shared<TextBlock>(std::move(words), std::move(positions), std::move(styles));
  Page page;
  page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, 20));
  return page;
}

ClippingCodec::ClippingMetadata clippingForWord(const uint16_t word, const uint32_t fingerprint) {
  ClippingCodec::ClippingMetadata clipping;
  clipping.spineIndex = 2;
  clipping.startPage = 3;
  clipping.endPage = 3;
  clipping.pageCount = 10;
  clipping.startWordIndex = word;
  clipping.endWordIndex = word;
  clipping.wordCount = 1;
  clipping.textLength = 1;
  clipping.pageFingerprint = fingerprint;
  return clipping;
}

}  // namespace

TEST(ClippingPageTools, BackClearsOnlyAnUncommittedSinglePageSelection) {
  EXPECT_FALSE(ClippingPageTools::shouldClearSelectionAnchorOnBack(-1, 0));
  EXPECT_TRUE(ClippingPageTools::shouldClearSelectionAnchorOnBack(0, 0));
  EXPECT_TRUE(ClippingPageTools::shouldClearSelectionAnchorOnBack(12, 0));
  EXPECT_FALSE(ClippingPageTools::shouldClearSelectionAnchorOnBack(0, 1));
}

TEST(ClippingPageTools, FingerprintCoversRenderContextAndAvailableLayoutData) {
  const Page page = makePage();
  const GfxRenderer renderer(480, 800, 24);
  const uint32_t baseline = ClippingPageTools::fingerprint(page, renderer, 101, 12, 18);
  ASSERT_NE(baseline, 0U);

  EXPECT_NE(ClippingPageTools::fingerprint(page, renderer, 102, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(page, GfxRenderer(480, 800, 25), 101, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(page, GfxRenderer(800, 480, 24), 101, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(page, renderer, 101, 13, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(makePage(9), renderer, 101, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(makePage(0, 4), renderer, 101, 12, 18), baseline);
  EXPECT_NE(ClippingPageTools::fingerprint(makePage(0, 0, true), renderer, 101, 12, 18), baseline);
}

TEST(ClippingPageTools, BuildsHighlightsOnlyForTheExactFingerprint) {
  const Page page = makePage();
  GfxRenderer renderer(480, 800, 24);
  const uint32_t fingerprint = ClippingPageTools::fingerprint(page, renderer, 101, 12, 18);

  ClippingCodec::ClippingMetadata clipping;
  clipping.spineIndex = 2;
  clipping.startPage = 3;
  clipping.endPage = 3;
  clipping.pageCount = 10;
  clipping.startWordIndex = 0;
  clipping.endWordIndex = 1;
  clipping.wordCount = 2;
  clipping.textLength = 1;
  clipping.pageFingerprint = fingerprint;

  const auto plan =
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 12, 18, {clipping}, 2, 3, fingerprint);
  ASSERT_EQ(plan.count, 2U);
  EXPECT_EQ(plan.lines[0].left, 17);
  EXPECT_EQ(plan.lines[0].y, 60);
  EXPECT_EQ(plan.lines[1].left, 47);

  EXPECT_EQ(
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 12, 18, {clipping}, 2, 3, fingerprint ^ 1U).count,
      0U);
}

TEST(ClippingPageTools, UsesFocusSplitGeometryForBoldPrefixAndRegularSuffix) {
  auto block = std::make_shared<TextBlock>(std::vector<std::string>{"abcdef"}, std::vector<int16_t>{0},
                                           std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR},
                                           std::vector<uint8_t>{2}, std::vector<uint16_t>{18});
  Page page;
  page.elements.push_back(std::make_shared<PageLine>(std::move(block), 5, 20));
  GfxRenderer renderer(480, 800, 24);
  const uint32_t fingerprint = ClippingPageTools::fingerprint(page, renderer, 101, 0, 0);

  const auto plan = ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0,
                                                               {clippingForWord(0, fingerprint)}, 2, 3, fingerprint);

  ASSERT_EQ(plan.count, 1U);
  EXPECT_EQ(plan.lines[0].left, 5);
  // The stub's regular suffix is 4 * 6 px; its bold-prefix layout offset is 18 px.
  EXPECT_EQ(plan.lines[0].right, 5 + 18 + 4 * 6 - 1);
}

TEST(ClippingPageTools, HighlightsTheSeventeenthAndAllSixtyFourStoredClippings) {
  const Page page = makeWordPage(ClippingCodec::MAX_CLIPPINGS_PER_BOOK);
  GfxRenderer renderer(480, 800, 24);
  const uint32_t pageFingerprint = ClippingPageTools::fingerprint(page, renderer, 101, 0, 0);
  std::vector<ClippingCodec::ClippingMetadata> clippings;
  clippings.reserve(ClippingCodec::MAX_CLIPPINGS_PER_BOOK);
  for (uint16_t word = 0; word < ClippingCodec::MAX_CLIPPINGS_PER_BOOK; ++word) {
    clippings.push_back(clippingForWord(word, pageFingerprint));
  }

  const auto plan =
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0, clippings, 2, 3, pageFingerprint);
  EXPECT_EQ(plan.count, ClippingCodec::MAX_CLIPPINGS_PER_BOOK);
  EXPECT_FALSE(plan.truncated);
  EXPECT_EQ(plan.lines[16].left, 5);

  clippings[16].pageFingerprint ^= 1U;
  const auto mismatched =
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0, clippings, 2, 3, pageFingerprint);
  EXPECT_EQ(mismatched.count, ClippingCodec::MAX_CLIPPINGS_PER_BOOK - 1);
}

TEST(ClippingPageTools, ReportsGeometryTruncationAtItsFixedMemoryLimit) {
  const Page page = makeWordPage(ClippingPageTools::HighlightPlan::MAX_LINES + 1);
  GfxRenderer renderer(480, 800, 24);
  const uint32_t pageFingerprint = ClippingPageTools::fingerprint(page, renderer, 101, 0, 0);
  auto clipping = clippingForWord(0, pageFingerprint);
  clipping.endWordIndex = static_cast<uint16_t>(ClippingPageTools::HighlightPlan::MAX_LINES);
  clipping.wordCount = static_cast<uint16_t>(ClippingPageTools::HighlightPlan::MAX_LINES + 1);

  const auto plan =
      ClippingPageTools::buildExactHighlightPlan(renderer, page, 101, 0, 0, {clipping}, 2, 3, pageFingerprint);
  EXPECT_EQ(plan.count, ClippingPageTools::HighlightPlan::MAX_LINES);
  EXPECT_TRUE(plan.truncated);
}

TEST(ClippingPageTools, TruncationNoticeIsOncePerRecentPageWithinFixedMemory) {
  ClippingPageTools::HighlightNoticeTracker tracker;
  EXPECT_TRUE(tracker.markIfNew(2, 3, 100));
  EXPECT_FALSE(tracker.markIfNew(2, 3, 100));
  EXPECT_TRUE(tracker.markIfNew(2, 3, 101));  // changed layout is a new identity
  EXPECT_TRUE(tracker.markIfNew(2, 4, 100));
  EXPECT_FALSE(tracker.markIfNew(0, 0, 0));

  for (size_t i = 3; i < ClippingPageTools::HighlightNoticeTracker::MAX_TRACKED_PAGES + 1; ++i) {
    EXPECT_TRUE(tracker.markIfNew(7, static_cast<uint16_t>(i), static_cast<uint32_t>(1000 + i)));
  }
  // The oldest identity can be reported again after the fixed-size ring has
  // evicted it; memory use never grows with page count.
  EXPECT_TRUE(tracker.markIfNew(2, 3, 100));
}

TEST(ClippingPageTools, PageAdvanceRequiresCompleteExtraction) {
  ClippingPageTools::SelectionPageAdvanceState state;
  state.loaderAvailable = true;
  state.extractionComplete = true;
  state.selectionStarted = true;
  state.wordCount = 192;
  state.cursorOrder = 191;
  state.currentPage = 3;
  state.pageCount = 5;
  state.selectionFits = true;
  EXPECT_TRUE(ClippingPageTools::canAdvanceSelectionPage(state));

  // A dense page may contain uncaptured words after the 192-word bounded
  // selection window. Advancing from that window would create a non-contiguous
  // clipping, so both the transition and its UI affordance must remain off.
  state.extractionComplete = false;
  EXPECT_FALSE(ClippingPageTools::canAdvanceSelectionPage(state));

  state.extractionComplete = true;
  state.cursorOrder = 190;
  EXPECT_FALSE(ClippingPageTools::canAdvanceSelectionPage(state));
  state.cursorOrder = 191;
  state.currentPage = 4;
  EXPECT_FALSE(ClippingPageTools::canAdvanceSelectionPage(state));
}

TEST(ClippingPageTools, CrossPageSelectionMustCoverAContiguousPhysicalPageTail) {
  const uint16_t completeTail[] = {18, 19, 20, 21};
  EXPECT_TRUE(ClippingPageTools::isContiguousTail(completeTail, std::size(completeTail), 22));

  const uint16_t hiddenMiddle[] = {18, 20, 21};
  EXPECT_FALSE(ClippingPageTools::isContiguousTail(hiddenMiddle, std::size(hiddenMiddle), 22));

  const uint16_t hiddenEnd[] = {18, 19, 20};
  EXPECT_FALSE(ClippingPageTools::isContiguousTail(hiddenEnd, std::size(hiddenEnd), 22));
  EXPECT_FALSE(ClippingPageTools::isContiguousTail(nullptr, 0, 22));
}

#include <GfxRenderer.h>
#include <gtest/gtest.h>

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

}  // namespace

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

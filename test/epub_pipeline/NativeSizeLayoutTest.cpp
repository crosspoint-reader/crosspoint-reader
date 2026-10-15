// A TTF family's sizes: every heading, CSS size and drop cap is laid out with a face of its own, at a whole number
// of pixels per em (FontSizeLadder::setNative), instead of the body face scaled.
//
// A built-in family has one auxiliary face per section (the decompressor's four page slots), so a second
// heading size is the body face scaled. A native face has no such budget: it is read from the glyph store.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/FontSizeLadder.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int32_t kNativeBase = 0x12345600;
constexpr uint32_t kBody14pt = (14 * 64 * 150 + 36) / 72;  // 29.17 px per em, 26.6

struct Line {
  std::string text;
  int32_t fontId;  // 0: the body face
  float mult;
};

struct NativeSizeLayoutFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_native_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // The first page's text lines, laid out with `ladder`.
  std::vector<Line> layout(const std::string& body, const FontSizeLadder& ladder) {
    const auto epub = syntheticBook(work, 0, body);
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    params.fontSizeLadder = ladder;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::vector<Line> lines;
    section.currentPage = 0;
    const auto page = section.loadPageFromSectionFile();
    if (!page) return lines;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
      if (block.wordCount() == 0) continue;
      Line line{{}, block.getRenderStyle().headingFontId, block.getRenderStyle().fontSizeMultiplier};
      for (uint16_t w = 0; w < block.wordCount(); ++w) {
        if (w > 0) line.text += ' ';
        line.text += block.wordText(w);
      }
      lines.push_back(line);
    }
    return lines;
  }

  static FontSizeLadder nativeLadder() {
    FontSizeLadder ladder;
    ladder.setNative(kNativeBase, kBody14pt, 10, 120);
    return ladder;
  }

  static const Line* find(const std::vector<Line>& lines, const std::string& text) {
    for (const auto& line : lines) {
      if (line.text == text) return &line;
    }
    return nullptr;
  }
};

}  // namespace

TEST_F(NativeSizeLayoutFixture, EachHeadingSizeHasAFaceOfItsOwn) {
  const auto lines = layout("<h1>Alpha</h1><h2>Bravo</h2><h3>Charlie</h3><p>Body text.</p>", nativeLadder());
  const Line* h1 = find(lines, "Alpha");
  const Line* h2 = find(lines, "Bravo");
  const Line* h3 = find(lines, "Charlie");
  const Line* p = find(lines, "Body text.");
  ASSERT_TRUE(h1 && h2 && h3 && p);
  // 1.6, 1.4 and 1.2 times 29.17 px: 47, 41 and 35 px, nothing left to scale.
  EXPECT_EQ(kNativeBase | 47, h1->fontId);
  EXPECT_EQ(kNativeBase | 41, h2->fontId);
  EXPECT_EQ(kNativeBase | 35, h3->fontId);
  for (const Line* heading : {h1, h2, h3}) EXPECT_FLOAT_EQ(1.0f, heading->mult) << heading->text;
  EXPECT_EQ(0, p->fontId);
  EXPECT_FLOAT_EQ(1.0f, p->mult);
}

TEST_F(NativeSizeLayoutFixture, ABuiltInLadderStillHasOneAuxiliaryFace) {
  FontSizeLadder rungs;  // Bookerly at 14 pt: 16 and 20 pt rungs
  rungs.addRung(104, 114);
  rungs.addRung(106, 142);
  const auto lines = layout("<h1>Alpha</h1><h3>Charlie</h3><p>Body text.</p>", rungs);
  const Line* h1 = find(lines, "Alpha");
  const Line* h3 = find(lines, "Charlie");
  ASSERT_TRUE(h1 && h3);
  EXPECT_EQ(106, h1->fontId);  // 160% on the 142% rung claims the section's one auxiliary face
  EXPECT_EQ(0, h3->fontId);    // 120% wants the 114% rung: taken, so the body face, scaled
  EXPECT_FLOAT_EQ(1.2f, h3->mult);
}

TEST_F(NativeSizeLayoutFixture, ACssSizeHasAFaceOfItsOwn) {
  const auto lines = layout("<p style=\"font-size: 0.8em\">Small print.</p><p>Body text.</p>", nativeLadder());
  const Line* small = find(lines, "Small print.");
  ASSERT_NE(nullptr, small);
  EXPECT_EQ(kNativeBase | 23, small->fontId);  // 23.3 px
  EXPECT_FLOAT_EQ(1.0f, small->mult);
}

TEST_F(NativeSizeLayoutFixture, ADropCapIsSetAtItsOwnSize) {
  const auto lines = layout(
      "<p><span style=\"float: left; font-size: 3em\">A</span>ll of this opening paragraph wraps beside a drop "
      "cap, line after line, until the cap is passed.</p>",
      nativeLadder());
  const Line* cap = find(lines, "A");
  ASSERT_NE(nullptr, cap);
  EXPECT_EQ(kNativeBase | 88, cap->fontId);  // 3 x 29.17 = 87.5 px
  EXPECT_FLOAT_EQ(1.0f, cap->mult);
}

TEST_F(NativeSizeLayoutFixture, ADropCapAboveTheLargestFaceIsItScaled) {
  FontSizeLadder ladder;
  ladder.setNative(kNativeBase, kBody14pt, 10, 60);
  const auto lines = layout(
      "<p><span style=\"float: left; font-size: 3em\">A</span>ll of this opening paragraph wraps beside a drop "
      "cap, line after line, until the cap is passed.</p>",
      ladder);
  const Line* cap = find(lines, "A");
  ASSERT_NE(nullptr, cap);
  EXPECT_EQ(kNativeBase | 60, cap->fontId);
  EXPECT_NEAR(87.52f / 60.0f, cap->mult, 0.002f);
}

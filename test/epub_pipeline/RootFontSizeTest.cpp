// The book's root font size (html/body) and the paragraphs sized against it.
//
// The reader's size setting is the size of the book's body text, so a size the book gives <body> or <html> is
// dropped (ChapterHtmlSlimParser::observeFontSizeBaseline). Calibre's conversions set the body to their base
// size and size every paragraph back down by a class, so that the product is exactly the book's default:
//
//   .calibre { font-size: 1.125em }    <body class="calibre">
//   .gt      { font-size: 0.88889em }  <p class="gt">...</p>
//
// Dropping only the body's factor left every paragraph at 0.889 of the chosen size, drawn as the body face
// shrunk by point sampling: jagged stems and lost pixel rows on every page of such a book (X4, 2026-10-19).
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

struct Line {
  std::string text;
  float mult;
};

struct RootFontSizeFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_rootsize_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // The first page's text lines of a chapter whose <body> has class "calibre", styled by `css`.
  std::vector<Line> layout(const std::string& css, const std::string& body) {
    const auto epub = syntheticBookFromDocument(
        work, 0,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body class=\"calibre\">\n" +
            body + "\n</body></html>\n",
        {{"style.css", "text/css", "p { margin: 0; text-indent: 0; }\n" + css}});
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
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
      Line line{{}, block.getRenderStyle().fontSizeMultiplier};
      for (uint16_t w = 0; w < block.wordCount(); ++w) {
        if (w > 0) line.text += ' ';
        line.text += block.wordText(w);
      }
      lines.push_back(line);
    }
    return lines;
  }
};

}  // namespace

TEST_F(RootFontSizeFixture, AParagraphThatUndoesTheBodySizeIsBodyText) {
  const auto lines =
      layout(".calibre { font-size: 1.125em; }\n.gt { font-size: 0.88889em; }\n", "<p class=\"gt\">Body text</p>");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_FLOAT_EQ(lines[0].mult, 1.0f) << "1.125em x 0.88889em is the book's own default size";
}

TEST_F(RootFontSizeFixture, AParagraphSmallerThanTheBodyStaysSmaller) {
  const auto lines =
      layout(".calibre { font-size: 1.125em; }\n.small { font-size: 0.7em; }\n", "<p class=\"small\">Small print</p>");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NEAR(lines[0].mult, 0.7f, 0.01f);
}

// Only an exact undo is body text: a note at 0.85em under a 1.2em body comes to 1.02 of the default, a size its
// designer chose below the body text's.
TEST_F(RootFontSizeFixture, AParagraphThatOnlyNearlyUndoesTheBodySizeKeepsItsSize) {
  const auto lines =
      layout(".calibre { font-size: 1.2em; }\n.note { font-size: 0.85em; }\n", "<p class=\"note\">A note</p>");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NEAR(lines[0].mult, 0.85f, 0.01f);
}

TEST_F(RootFontSizeFixture, WithoutABodySizeAParagraphKeepsItsOwn) {
  const auto lines = layout(".gt { font-size: 0.88889em; }\n", "<p class=\"gt\">Body text</p>");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NEAR(lines[0].mult, 0.889f, 0.01f);
}

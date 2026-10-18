// CSS page-break-after (and break-after) starts the content after the element on a new page, and
// page-break-before breaks between the element and the paragraph before it.
//
// The CSS parser stored both, but only page-break-before was ever acted on: a book that ends each
// chapter, title page or poem with page-break-after:always (Calibre and InDesign exports do) ran
// the next part on straight after it. And page-break-before was taken at the start tag, when the
// paragraph before the element had not been laid out yet: the page was emitted without it, and it
// went over to the new page with the element.
//
// What has to hold:
//   1. always/page on a paragraph, a heading or a <div> puts what follows on a new page; the
//      element's own content stays where it was.
//   2. A break-after on the last element leaves no empty page behind.
//   3. A break-after followed by a break-before is one break, not two.
//   4. Nested elements that both break after each get their break.
//   5. A break-before keeps the paragraph before the element on the old page.
//   6. An image after a break-after starts the new page.
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

struct Layout {
  uint16_t pages = 0;
  std::map<std::string, uint16_t> pageOf;  // each word's page (every word in a case is unique)
  int imagePage = -1;                      // the page of the (only) image, -1 when none
};

struct PageBreakAfterFixture : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_pagebreakafter_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  Layout layout(const std::string& css, const std::string& body) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
        body + "\n</body></html>\n";
    auto epub = syntheticBookFromDocument(
        work, books++, doc, {{"style.css", "text/css", css}, {"pic.png", "image/png", syntheticPngHeader(200, 100)}});
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    Layout out;
    out.pages = section.pageCount;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const auto& el : page->elements) {
        if (el->getTag() == TAG_PageImage) out.imagePage = p;
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
        for (uint16_t w = 0; w < block.wordCount(); ++w) out.pageOf[block.wordText(w)] = p;
      }
    }
    return out;
  }
};

}  // namespace

TEST_F(PageBreakAfterFixture, ParagraphBreaksAfterItself) {
  const auto l = layout("", "<p>First</p><p style=\"page-break-after:always\">Second</p><p>Third</p>");
  EXPECT_EQ(l.pages, 2);
  EXPECT_EQ(l.pageOf.at("First"), 0);
  EXPECT_EQ(l.pageOf.at("Second"), 0) << "the element's own content moved instead of what follows it";
  EXPECT_EQ(l.pageOf.at("Third"), 1);
}

TEST_F(PageBreakAfterFixture, BreakAfterFromAClassOnADivAndAHeading) {
  const auto l = layout(".end { break-after: page; } h2 { page-break-after: always; }",
                        "<div class=\"end\"><p>Alpha</p><p>Bravo</p></div><h2>Heading</h2><p>Body</p>");
  EXPECT_EQ(l.pages, 3);
  EXPECT_EQ(l.pageOf.at("Alpha"), 0);
  EXPECT_EQ(l.pageOf.at("Bravo"), 0);
  EXPECT_EQ(l.pageOf.at("Heading"), 1);
  EXPECT_EQ(l.pageOf.at("Body"), 2);
}

TEST_F(PageBreakAfterFixture, LastElementLeavesNoEmptyPage) {
  const auto l = layout("", "<p>Only</p><p style=\"page-break-after:always\">Closing</p>");
  EXPECT_EQ(l.pages, 1);
}

TEST_F(PageBreakAfterFixture, BreakAfterThenBreakBeforeIsOneBreak) {
  const auto l =
      layout("", "<p style=\"page-break-after:always\">Before</p><p style=\"page-break-before:always\">After</p>");
  EXPECT_EQ(l.pages, 2);
  EXPECT_EQ(l.pageOf.at("After"), 1);
}

TEST_F(PageBreakAfterFixture, NestedBreaksEachApply) {
  const auto l = layout("",
                        "<div style=\"page-break-after:always\"><p style=\"page-break-after:always\">Inner</p>"
                        "<p>Sibling</p></div><p>Outside</p>");
  EXPECT_EQ(l.pages, 3);
  EXPECT_EQ(l.pageOf.at("Inner"), 0);
  EXPECT_EQ(l.pageOf.at("Sibling"), 1);
  EXPECT_EQ(l.pageOf.at("Outside"), 2);
}

TEST_F(PageBreakAfterFixture, BreakBeforeKeepsThePreviousParagraphBehind) {
  const auto l = layout("", "<p>Zero</p><p>One</p><p style=\"page-break-before:always\">Two</p>");
  EXPECT_EQ(l.pages, 2);
  EXPECT_EQ(l.pageOf.at("Zero"), 0);
  EXPECT_EQ(l.pageOf.at("One"), 0) << "the paragraph before the break went over with the element";
  EXPECT_EQ(l.pageOf.at("Two"), 1);
}

TEST_F(PageBreakAfterFixture, ImageAfterABreakStartsTheNewPage) {
  const auto l =
      layout("", "<p style=\"page-break-after:always\">Caption</p><div><img src=\"pic.png\" alt=\"\"/></div>");
  EXPECT_EQ(l.pageOf.at("Caption"), 0);
  EXPECT_EQ(l.imagePage, 1);
}

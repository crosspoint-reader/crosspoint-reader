// Bold and italic set on a block reach the blocks nested inside it.
//
//   <div style="font-style:italic"><p>Inner</p>tail</div>
//
// renders "Inner" and "tail" italic in every reader: font-style and font-weight are inherited. The
// parser kept a block's own CSS in one slot that every nested block overwrote and every block close
// cleared, so the <p> above came out upright, and so did "tail" after it. Only containers that are
// not BLOCK_TAGS (<section>, <article>, <ul>) pushed an entry onto the inline style stack, which
// applies AFTER the block slot: an italic <section> also overrode its own <p style="font-style:
// normal">. A <br> inside an italic <p> closed as a block too and cleared the paragraph's italic for
// the rest of it.
//
// What has to hold:
//   1. A block's bold/italic reaches the blocks and text nested inside it (div, li, blockquote, pre,
//      section), and the text after a nested block, up to the block's own end tag.
//   2. A nested block that sets font-style:normal / font-weight:normal overrides it, and the
//      parent's style returns once that child closes.
//   3. Nothing leaks past the container's end tag.
//   4. Headings stay bold, and take an enclosing block's italic too.
//   5. A <br> does not end the paragraph's own style.
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>

#include "EpdFontFamily.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr uint8_t kBold = EpdFontFamily::BOLD;
constexpr uint8_t kItalic = EpdFontFamily::ITALIC;
constexpr uint8_t kEmphasis = kBold | kItalic;

struct BlockEmphasisFixture : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_blockemph_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // Bold/italic bits of every word of the chapter, keyed by its text (each word is unique).
  std::map<std::string, uint8_t> styles(const std::string& css, const std::string& body) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
        body + "\n</body></html>\n";
    auto epub = syntheticBookFromDocument(work, books++, doc, {{"style.css", "text/css", css}});
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::map<std::string, uint8_t> out;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
        for (uint16_t w = 0; w < block.wordCount(); ++w) {
          out[block.wordText(w)] = static_cast<uint8_t>(block.wordStyle(w)) & kEmphasis;
        }
      }
    }
    return out;
  }
};

}  // namespace

TEST_F(BlockEmphasisFixture, DivItalicReachesNestedParagraphAndTail) {
  const auto s = styles("", "<div style=\"font-style:italic\"><p>Inner</p>Tail</div><p>After</p>");
  EXPECT_EQ(s.at("Inner"), kItalic);
  EXPECT_EQ(s.at("Tail"), kItalic);
  EXPECT_EQ(s.at("After"), 0) << "the div's italic leaked past </div>";
}

TEST_F(BlockEmphasisFixture, ChildOverridesAndParentIsRestored) {
  const auto s = styles(".it { font-style: italic; } .up { font-style: normal; }",
                        "<div class=\"it\"><p class=\"up\">Upright</p><p>Slanted</p></div>");
  EXPECT_EQ(s.at("Upright"), 0);
  EXPECT_EQ(s.at("Slanted"), kItalic);
}

TEST_F(BlockEmphasisFixture, BoldListItemReachesItsParagraph) {
  const auto s = styles("li { font-weight: bold; }", "<ul><li><p>Item</p></li></ul><p>Plain</p>");
  EXPECT_EQ(s.at("Item"), kBold);
  EXPECT_EQ(s.at("Plain"), 0);
}

TEST_F(BlockEmphasisFixture, BlockquoteAndPreReachTheirChildren) {
  const auto s = styles("blockquote { font-style: italic; } pre { font-weight: bold; }",
                        "<blockquote><p>Quoted</p></blockquote><pre><span>Code</span></pre>"
                        "<div style=\"font-weight:bold\"><pre>Nested</pre></div><p>Done</p>");
  EXPECT_EQ(s.at("Quoted"), kItalic);
  EXPECT_EQ(s.at("Code"), kBold);
  EXPECT_EQ(s.at("Nested"), kBold);
  EXPECT_EQ(s.at("Done"), 0);
}

TEST_F(BlockEmphasisFixture, SectionChildCanSetNormal) {
  const auto s = styles("section { font-style: italic; } .up { font-style: normal; }",
                        "<section><p class=\"up\">Roman</p><p>Oblique</p></section>");
  EXPECT_EQ(s.at("Roman"), 0) << "the <section>'s italic overrode the paragraph's own font-style:normal";
  EXPECT_EQ(s.at("Oblique"), kItalic);
}

TEST_F(BlockEmphasisFixture, HeadingStaysBoldAndTakesEnclosingItalic) {
  const auto s = styles("", "<div style=\"font-style:italic\"><h2>Heading</h2><p>Body</p></div><h3>Plainhead</h3>");
  EXPECT_EQ(s.at("Heading"), kEmphasis);
  EXPECT_EQ(s.at("Body"), kItalic);
  EXPECT_EQ(s.at("Plainhead"), kBold);
}

TEST_F(BlockEmphasisFixture, BrKeepsTheParagraphsOwnStyle) {
  const auto s = styles("", "<p style=\"font-style:italic\">Before<br/>Below</p><p>Next</p>");
  EXPECT_EQ(s.at("Before"), kItalic);
  EXPECT_EQ(s.at("Below"), kItalic) << "the <br> cleared the paragraph's italic";
  EXPECT_EQ(s.at("Next"), 0);
}

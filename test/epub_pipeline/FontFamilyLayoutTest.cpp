// A book's other font families (Publisher Fonts' Accents and All Text, BlockFontFamilies): a block whose CSS
// font-family is one of them is laid out with that family's face at its size, through the block font ID; a generic
// sans-serif goes to a built-in family; the body family, monospace and families the book does not have keep the body
// font. Without families nothing about a block's font changes (Off and Body Text).
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/BlockFontFamilies.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/css/CssParser.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int32_t kAccentBase = 0x12345600;  // the accent family's native sizes
constexpr int32_t kAccentBody = 0x777;       // its face at the body size
constexpr uint32_t kBody14pt = (14 * 64 * 150 + 36) / 72;
constexpr int32_t kSansBody = 2014;  // the built-in sans-serif at 14 pt; its rungs are 2000 + their size

const char* kCss =
    "body { font-family: 'Book Body', serif }\n"
    "h1, .dedication, blockquote { font-family: Display, sans-serif }\n"
    "h2.generic, .sans { font-family: sans-serif }\n"
    ".body2 { font-family: 'book body', sans-serif }\n"
    ".helvetica { font-family: Helvetica, Arial, sans-serif }\n"
    ".georgia { font-family: Georgia, serif }\n"
    ".mono { font-family: Courier, monospace }\n";

struct Line {
  std::string text;
  int32_t fontId;  // 0: the body face
  float mult;
};

struct FontFamilyLayoutFixture : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_families_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // The first page's text lines, laid out with `families` (nullptr: none).
  std::vector<Line> layout(const std::string& body, const BlockFontFamilies* families) {
    const auto epub = syntheticBookFromDocument(
        work, books++,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" href=\"style.css\" type=\"text/css\"/></head><body>\n" +
            body + "\n</body></html>\n",
        {{"style.css", "text/css", kCss}});
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    params.fontFamilies = families;
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

  // Accents with a built-in reader font: the book's accent family loaded, sans-serif on the built-in sans, serif
  // left out as the body's own generic.
  static BlockFontFamilies accents() {
    BlockFontFamilies families;
    families.body = CssParser::fontFamilyHash("Book Body") | CssFontFamily::kSerif;
    FontSizeLadder sans;
    for (const uint16_t pt : {10, 12, 14, 16, 18, 20, 22, 24, 26}) sans.addRung(2000 + pt, pt * 100 / 14);
    families.add(CssFontFamily::kSansSerif, kSansBody, sans);
    FontSizeLadder native;
    native.setNative(kAccentBase, kBody14pt, 10, 120);
    families.add(CssParser::fontFamilyHash("Display"), kAccentBody, native);
    return families;
  }

  static const Line* find(const std::vector<Line>& lines, const std::string& text) {
    for (const auto& line : lines) {
      if (line.text == text) return &line;
    }
    return nullptr;
  }
};

}  // namespace

TEST_F(FontFamilyLayoutFixture, WithoutFamiliesABlocksFontIsAsBefore) {
  const auto lines = layout("<h1>Alpha</h1><p class='dedication'>For someone</p><p>Body text.</p>", nullptr);
  const Line* h1 = find(lines, "Alpha");
  const Line* dedication = find(lines, "For someone");
  ASSERT_TRUE(h1 && dedication);
  EXPECT_EQ(0, h1->fontId);  // no ladder: the body face scaled
  EXPECT_FLOAT_EQ(1.6f, h1->mult);
  EXPECT_EQ(0, dedication->fontId);
  EXPECT_FLOAT_EQ(1.0f, dedication->mult);
}

TEST_F(FontFamilyLayoutFixture, AHeadingInTheAccentFamilyHasItsFaceAtItsSize) {
  const BlockFontFamilies families = accents();
  const auto lines = layout("<h1>Alpha</h1><p>Body text.</p>", &families);
  const Line* h1 = find(lines, "Alpha");
  const Line* p = find(lines, "Body text.");
  ASSERT_TRUE(h1 && p);
  EXPECT_EQ(kAccentBase | 47, h1->fontId);  // 1.6 x 29.17 px
  EXPECT_FLOAT_EQ(1.0f, h1->mult);
  EXPECT_EQ(0, p->fontId);
}

TEST_F(FontFamilyLayoutFixture, ABodySizeBlockInAnotherFamilyHasThatFamilysBodyFace) {
  const BlockFontFamilies families = accents();
  const auto lines = layout("<p class='dedication'>For someone</p><p class='sans'>Sans words</p>", &families);
  const Line* dedication = find(lines, "For someone");
  const Line* sans = find(lines, "Sans words");
  ASSERT_TRUE(dedication && sans);
  EXPECT_EQ(kAccentBody, dedication->fontId);
  EXPECT_FLOAT_EQ(1.0f, dedication->mult);
  EXPECT_EQ(kSansBody, sans->fontId);
  EXPECT_FLOAT_EQ(1.0f, sans->mult);
}

TEST_F(FontFamilyLayoutFixture, AGenericSansHeadingHasTheNearestBuiltInRung) {
  const BlockFontFamilies families = accents();
  const auto lines = layout("<h2 class='generic'>Bravo</h2>", &families);
  const Line* h2 = find(lines, "Bravo");
  ASSERT_NE(nullptr, h2);
  EXPECT_EQ(2020, h2->fontId);      // 140 % on the 142 % rung
  EXPECT_FLOAT_EQ(1.0f, h2->mult);  // inside the dead zone
}

TEST_F(FontFamilyLayoutFixture, TheBodyFamilyAndFamiliesTheBookLacksKeepTheBodyFont) {
  const BlockFontFamilies families = accents();
  const auto lines = layout(
      "<p class='body2'>Body again</p><p class='georgia'>Georgia words</p><p class='mono'>Mono words</p>"
      "<p class='helvetica'>Helvetica words</p>",
      &families);
  const Line* body2 = find(lines, "Body again");
  const Line* georgia = find(lines, "Georgia words");
  const Line* mono = find(lines, "Mono words");
  const Line* helvetica = find(lines, "Helvetica words");
  ASSERT_TRUE(body2 && georgia && mono && helvetica);
  EXPECT_EQ(0, body2->fontId);              // the body family, whatever generic its list ends in
  EXPECT_EQ(0, georgia->fontId);            // serif is the body's own generic: the body font stands for it
  EXPECT_EQ(0, mono->fontId);               // no monospace face is built in
  EXPECT_EQ(kSansBody, helvetica->fontId);  // not in the book: its list's generic
}

TEST_F(FontFamilyLayoutFixture, BesideATrueTypeReaderFontBothGenericsAndTheAccentFamilyServe) {
  // A book whose body names no generic, beside a TrueType reader font: neither built-in family is the body's, so
  // serif and sans-serif both map onto theirs, and the accent family loads beside the reader font (Stage B phase 2).
  BlockFontFamilies families = accents();
  families.body = CssParser::fontFamilyHash("Book Body");
  FontSizeLadder serif;
  for (const uint16_t pt : {10, 12, 14, 16, 18, 20, 22, 24, 26}) serif.addRung(1000 + pt, pt * 100 / 14);
  families.add(CssFontFamily::kSerif, 1014, serif);
  ASSERT_EQ(3, families.count);
  const auto lines = layout(
      "<p class='georgia'>Georgia words</p><p class='helvetica'>Helvetica words</p>"
      "<p class='dedication'>For someone</p>",
      &families);
  const Line* georgia = find(lines, "Georgia words");
  const Line* helvetica = find(lines, "Helvetica words");
  const Line* dedication = find(lines, "For someone");
  ASSERT_TRUE(georgia && helvetica && dedication);
  EXPECT_EQ(1014, georgia->fontId);
  EXPECT_EQ(kSansBody, helvetica->fontId);
  EXPECT_EQ(kAccentBody, dedication->fontId);
}

TEST_F(FontFamilyLayoutFixture, ABlockTakesItsWordsFamilyWhenAllAreInOne) {
  const BlockFontFamilies families = accents();
  const auto lines = layout(
      "<p><span class='dedication'>All of it</span></p><p>Body <span class='dedication'>accent</span> body</p>"
      "<blockquote><p>Quoted</p></blockquote>",
      &families);
  const Line* span = find(lines, "All of it");
  const Line* mixed = find(lines, "Body accent body");
  const Line* quoted = find(lines, "Quoted");
  ASSERT_TRUE(span && mixed && quoted);
  EXPECT_EQ(kAccentBody, span->fontId);
  EXPECT_EQ(0, mixed->fontId);             // several families: its own element's
  EXPECT_EQ(kAccentBody, quoted->fontId);  // inherited from the blockquote
}

TEST_F(FontFamilyLayoutFixture, ALongBlockLaidOutInPartsKeepsItsFamily) {
  const BlockFontFamilies families = accents();
  std::string words;
  for (int i = 0; i < 130; ++i) words += "word ";
  const auto lines = layout("<p class='dedication'>" + words + "</p>", &families);
  ASSERT_GT(lines.size(), 3u);
  for (const Line& line : lines) EXPECT_EQ(kAccentBody, line.fontId) << line.text;
}

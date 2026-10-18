// KOReader <-> CrossPoint position mapping on a controlled three-chapter book.
//
// A KOSync record carries two positions: an XPath and a book percentage. They measure different
// things. The XPath names an element in the chapter; the percentage is KOReader's rendered page
// over its page count, so images, page breaks and CSS all move it. Our own book percentage is
// inflated-XHTML bytes. A gap of a percent or two between the two percentages is normal.
//
// The book:
//   spine 0  ch12  40 paragraphs as direct children of <body>  (the shape the paragraph LUT keys on);
//                  the first ends in an entity, &mdash;
//   spine 1  ch13  a linked chapter number in <section><header><hgroup><h1><span><a><span>, then
//                  60 paragraphs inside the <section>  (the reporter's book in issue #268)
//   spine 2  ch14  60 body-child paragraphs, then a tag that is not XML, so the XPath resolver fails
//                  on this chapter the way it fails on the device when the inflate ring cannot be
//                  allocated, and the mapper has to fall back to the percentage
//
// Three groups of cases:
//   1. The resolver succeeds (#268). An exact match is used as is, whatever the percentage says.
//      An inexact match (an ancestor stands in for an element this parse does not have) still
//      defers to the percentage, because the reverse mapper anchors a text-less wrapper at its
//      END tag and would otherwise land on the chapter's last page.
//   2. The resolver fails. The spine still comes from the XPath; the page comes from the
//      percentage, clamped to that spine, except for a chapter-start XPath, which pins page 0.
//   3. The XPath carries no usable spine. Everything comes from the percentage.
// Plus the forward direction's edges: first page, last page, one-page and zero-page chapters.
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "KOReaderSync/ChapterXPathIndexer.h"
#include "KOReaderSync/ChapterXPathIndexerInternal.h"
#include "KOReaderSync/ProgressMapper.h"
#include "StoredZipWriter.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int kFlatSpine = 0;      // DocFragment[1]
constexpr int kSectionSpine = 1;   // DocFragment[2]
constexpr int kBrokenSpine = 2;    // DocFragment[3]
constexpr int kPagesInSpine = 40;  // the reader's page count for whichever chapter is current

// `tail` is appended to the first paragraph's text.
std::string paragraphs(const char* label, const int count, const char* tail = "") {
  std::string out;
  for (int i = 1; i <= count; ++i) {
    out += "<p>" + std::string(label) + " paragraph " + std::to_string(i) +
           " carries enough ordinary words to give it some weight on the page, and then a second"
           " clause long enough that a page can end inside it." +
           (i == 1 ? tail : "") + "</p>\n";
  }
  return out;
}

std::string xhtml(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
         "<head><title>C</title></head><body>\n" +
         body + "\n</body></html>\n";
}

struct ProgressMapperFixture : testing::Test {
  fs::path work;
  std::shared_ptr<Epub> epub;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("progress_mapper_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);

    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">progressmapper</dc:identifier>"
            "<dc:title>Progress mapper</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n"
            "<item id=\"c1\" href=\"ch12.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "<item id=\"c2\" href=\"ch13.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "<item id=\"c3\" href=\"ch14.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "</manifest>\n<spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/><itemref idref=\"c3\"/></spine>\n"
            "</package>\n");
    zip.add("ch12.xhtml", xhtml(paragraphs("Earlier", 40, " &mdash;")));
    zip.add("ch13.xhtml", xhtml("<section><header><hgroup><h1><span><a href=\"toc.xhtml\"><span>Chapter 13</span></a>"
                                "</span><span>The Title</span></h1></hgroup></header>\n" +
                                paragraphs("Target", 60) + "</section>"));
    // "<" followed by a space is not XML: the SAX parser stops with an error, the reverse mapper
    // reports no match, and ProgressMapper takes its no-resolver path.
    zip.add("ch14.xhtml", xhtml(paragraphs("Broken", 60) + "<p>a < b</p>"));
    const std::string path = (work / "book.epub").string();
    zip.write(path);

    epub = std::make_shared<Epub>(path, (work / "cache").string());
    ASSERT_TRUE(epub->load(true));
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work);
  }

  // The book percentage of a point `fraction` of the way through `spine`, by bytes. fraction may
  // run below 0 or above 1 to reach into the neighbouring chapters.
  float percentageInto(const int spine, const float fraction) const {
    const float start = spine > 0 ? static_cast<float>(epub->getCumulativeSpineItemSize(spine - 1)) : 0.0f;
    const float size = static_cast<float>(epub->getCumulativeSpineItemSize(spine)) - start;
    return (start + fraction * size) / static_cast<float>(epub->getBookSize());
  }

  // Maps as the sync activity does: the reader is open on `currentSpine` with kPagesInSpine pages.
  CrossPointPosition map(const std::string& xpath, const float percentage,
                         const int currentSpine = kSectionSpine) const {
    return ProgressMapper::toCrossPoint(epub, {xpath, percentage}, currentSpine, kPagesInSpine);
  }
};

// --- 1. The resolver succeeds -------------------------------------------------------------------

TEST_F(ProgressMapperFixture, ChapterHeadingStaysOnFirstPageWhenPercentageRunsAhead) {
  const float percentage = percentageInto(kSectionSpine, 0.3f);
  // The scenario only means something if the old reconcile would have fired: more than 1% of the
  // book past the chapter's start, inside the same spine.
  ASSERT_GT(percentage - percentageInto(kSectionSpine, 0.0f), 0.01f);

  const auto pos = map("/body/DocFragment[2]/body/section/header/hgroup/h1/span[1]/a/span/text().0", percentage);

  EXPECT_EQ(pos.spineIndex, kSectionSpine);
  EXPECT_EQ(pos.pageNumber, 0);
}

TEST_F(ProgressMapperFixture, MidChapterXPathWinsOverDisagreeingPercentage) {
  const float percentage = percentageInto(kSectionSpine, 0.05f);

  // p[30] of 60 equal paragraphs: just under half way, so about page 19 of 40.
  const auto pos = map("/body/DocFragment[2]/body/section/p[30]/text().0", percentage);

  ASSERT_GT(percentageInto(kSectionSpine, 0.48f) - percentage, 0.01f);
  EXPECT_EQ(pos.spineIndex, kSectionSpine);
  EXPECT_NEAR(pos.pageNumber, 19, 2);
}

TEST_F(ProgressMapperFixture, ExactXPathWinsOverPercentageInAnotherSpine) {
  // KOReader's percentage says the previous chapter; the XPath says this one. The XPath names
  // an element, the percentage is an estimate: the element wins.
  const float percentage = percentageInto(kFlatSpine, 0.5f);

  const auto pos = map("/body/DocFragment[2]/body/section/p[30]/text().0", percentage);

  EXPECT_EQ(pos.spineIndex, kSectionSpine);
  EXPECT_NEAR(pos.pageNumber, 19, 2);
}

TEST_F(ProgressMapperFixture, ExactXPathIgnoresAnUnusablePercentage) {
  const auto pos = map("/body/DocFragment[2]/body/section/header/hgroup/h1/span[1]/a/span/text().0",
                       std::numeric_limits<float>::quiet_NaN());

  EXPECT_EQ(pos.spineIndex, kSectionSpine);
  EXPECT_EQ(pos.pageNumber, 0);
}

TEST_F(ProgressMapperFixture, AncestorOnlyMatchStillDefersToPercentage) {
  const float percentage = percentageInto(kSectionSpine, 0.4f);

  // There is no div in this chapter, so only the enclosing section matches, and at its END tag.
  const auto pos = map("/body/DocFragment[2]/body/section/div[4]/p[2]/text().0", percentage);

  EXPECT_EQ(pos.spineIndex, kSectionSpine);
  EXPECT_NEAR(pos.pageNumber, 16, 1);  // 0.4 of 40 pages, not the last page
}

TEST_F(ProgressMapperFixture, AncestorOnlyMatchKeepsTheXPathSpineWhenPercentageIsElsewhere) {
  // The percentage can only stand in for the page when it falls in the XPath's own spine. Here it
  // does not, so the spine is still the XPath's; the page is whatever the ancestor resolved to.
  const float percentage = percentageInto(kFlatSpine, 0.5f);

  const auto pos = map("/body/DocFragment[2]/body/section/div[4]/p[2]/text().0", percentage);

  EXPECT_EQ(pos.spineIndex, kSectionSpine);
}

TEST_F(ProgressMapperFixture, BodyChildParagraphXPathCarriesTheParagraphIndex) {
  // The reader snaps to the page where p[25] starts through the section cache's paragraph LUT,
  // which only knows direct children of <body>. The mapper has to hand the index over.
  const auto pos = map("/body/DocFragment[1]/body/p[25]/text().0", percentageInto(kFlatSpine, 0.6f), kFlatSpine);

  EXPECT_EQ(pos.spineIndex, kFlatSpine);
  EXPECT_TRUE(pos.hasParagraphIndex);
  EXPECT_EQ(pos.paragraphIndex, 25);
}

TEST_F(ProgressMapperFixture, NestedParagraphXPathCarriesNoParagraphIndex) {
  // p[30] inside <section> is not the 30th <p> child of <body>, so the LUT could not use it.
  const auto pos = map("/body/DocFragment[2]/body/section/p[30]/text().0", percentageInto(kSectionSpine, 0.5f));

  EXPECT_FALSE(pos.hasParagraphIndex);
}

// --- 2. The resolver fails ----------------------------------------------------------------------

TEST_F(ProgressMapperFixture, UnresolvedChapterStartPinsFirstPage) {
  const float percentage = percentageInto(kBrokenSpine, 0.3f);

  const auto pos = map("/body/DocFragment[3]/body/h1/text().0", percentage);

  EXPECT_EQ(pos.spineIndex, kBrokenSpine);
  EXPECT_EQ(pos.pageNumber, 0);
}

TEST_F(ProgressMapperFixture, UnresolvedChapterStartKeepsTheXPathSpineWhenPercentageIsElsewhere) {
  const float percentage = percentageInto(kFlatSpine, 0.5f);

  const auto pos = map("/body/DocFragment[3]/body/h1/text().0", percentage);

  EXPECT_EQ(pos.spineIndex, kBrokenSpine);
  EXPECT_EQ(pos.pageNumber, 0);
}

TEST_F(ProgressMapperFixture, UnresolvedParagraphTakesThePageFromThePercentage) {
  const float percentage = percentageInto(kBrokenSpine, 0.4f);

  const auto pos = map("/body/DocFragment[3]/body/p[30]/text().0", percentage, kBrokenSpine);

  EXPECT_EQ(pos.spineIndex, kBrokenSpine);
  EXPECT_NEAR(pos.pageNumber, 16, 1);  // 0.4 of 40 pages
  // The index is string-derived, so it survives the failed resolve and the reader can still snap.
  EXPECT_TRUE(pos.hasParagraphIndex);
  EXPECT_EQ(pos.paragraphIndex, 30);
}

TEST_F(ProgressMapperFixture, UnresolvedParagraphWithPercentageBeforeTheSpineClampsToFirstPage) {
  const float percentage = percentageInto(kBrokenSpine, -0.5f);

  const auto pos = map("/body/DocFragment[3]/body/p[30]/text().0", percentage, kBrokenSpine);

  EXPECT_EQ(pos.spineIndex, kBrokenSpine);
  EXPECT_EQ(pos.pageNumber, 0);
}

TEST_F(ProgressMapperFixture, UnresolvedParagraphWithPercentageBeyondTheSpineClampsToLastPage) {
  const auto pos = map("/body/DocFragment[3]/body/p[30]/text().0", 1.5f, kBrokenSpine);

  EXPECT_EQ(pos.spineIndex, kBrokenSpine);
  EXPECT_EQ(pos.pageNumber, kPagesInSpine - 1);
}

TEST_F(ProgressMapperFixture, UnresolvedParagraphWithUnusablePercentageLandsOnFirstPage) {
  const auto pos =
      map("/body/DocFragment[3]/body/p[30]/text().0", std::numeric_limits<float>::quiet_NaN(), kBrokenSpine);

  EXPECT_EQ(pos.spineIndex, kBrokenSpine);
  EXPECT_EQ(pos.pageNumber, 0);
  EXPECT_TRUE(pos.hasParagraphIndex);
}

// --- 3. No usable spine in the XPath ------------------------------------------------------------

TEST_F(ProgressMapperFixture, NoDocFragmentFallsBackToThePercentageSpine) {
  const auto pos = map("/body/p[3]", percentageInto(kSectionSpine, 0.5f));

  EXPECT_EQ(pos.spineIndex, kSectionSpine);
  EXPECT_NEAR(pos.pageNumber, 20, 1);
  EXPECT_FALSE(pos.hasParagraphIndex);
}

TEST_F(ProgressMapperFixture, EmptyXPathFallsBackToThePercentage) {
  const auto pos = map("", percentageInto(kFlatSpine, 0.5f));

  EXPECT_EQ(pos.spineIndex, kFlatSpine);
}

TEST_F(ProgressMapperFixture, DocFragmentOutOfRangeFallsBackToThePercentage) {
  const auto pos = map("/body/DocFragment[99]/body/p[1]", percentageInto(kFlatSpine, 0.5f));

  EXPECT_EQ(pos.spineIndex, kFlatSpine);
}

TEST_F(ProgressMapperFixture, DocFragmentZeroIsNotASpine) {
  // XPath predicates are 1-based; [0] is not a chapter.
  const auto pos = map("/body/DocFragment[0]/body/p[1]", percentageInto(kSectionSpine, 0.5f));

  EXPECT_EQ(pos.spineIndex, kSectionSpine);
  EXPECT_FALSE(pos.hasParagraphIndex);
}

TEST_F(ProgressMapperFixture, PercentageZeroIsTheStartOfTheBook) {
  const auto pos = map("", 0.0f);

  EXPECT_EQ(pos.spineIndex, 0);
  EXPECT_EQ(pos.pageNumber, 0);
}

TEST_F(ProgressMapperFixture, PercentageOneIsTheLastPageOfTheLastSpine) {
  const auto pos = map("", 1.0f, kBrokenSpine);

  EXPECT_EQ(pos.spineIndex, kBrokenSpine);
  EXPECT_EQ(pos.pageNumber, kPagesInSpine - 1);
}

TEST_F(ProgressMapperFixture, PercentageAtASpineBoundaryIsTheEndOfTheEarlierSpine) {
  // The byte exactly at the boundary belongs to both "the end of 12" and "the start of 13".
  // Either would be right; this pins which one, so a change is noticed.
  const auto pos = map("", percentageInto(kFlatSpine, 1.0f), kFlatSpine);

  EXPECT_EQ(pos.spineIndex, kFlatSpine);
  EXPECT_EQ(pos.pageNumber, kPagesInSpine - 1);
}

TEST_F(ProgressMapperFixture, UnusablePercentageAndNoXPathGiveTheStartOfTheBook) {
  const auto pos = map("", std::numeric_limits<float>::quiet_NaN());

  EXPECT_EQ(pos.spineIndex, 0);
  EXPECT_EQ(pos.pageNumber, 0);
}

// --- The forward direction's edges --------------------------------------------------------------

TEST_F(ProgressMapperFixture, FirstPageUploadsAsTheChapterStart) {
  const auto ko = ProgressMapper::toKOReader(epub, {kSectionSpine, 0, kPagesInSpine});

  EXPECT_FLOAT_EQ(ko.percentage, percentageInto(kSectionSpine, 0.0f));
  EXPECT_EQ(ko.xpath.rfind("/body/DocFragment[2]/body/", 0), 0u) << ko.xpath;

  const auto back = ProgressMapper::toCrossPoint(epub, ko, kSectionSpine, kPagesInSpine);
  EXPECT_EQ(back.spineIndex, kSectionSpine);
  EXPECT_EQ(back.pageNumber, 0);
}

TEST_F(ProgressMapperFixture, LastPageUploadsAsTheLastParagraphAndComesBackThere) {
  // The two passes of the forward mapper count the chapter's text separately. If they ever
  // disagreed, the last page's target would be unreachable, the XPath would fall back to the
  // chapter's root, and the position would come back as page 0. What it does come back as is
  // the first page of the last paragraph: an upload names the paragraph, not the character,
  // and 60 paragraphs over 40 pages put p[60]'s start on page 38.
  const auto ko = ProgressMapper::toKOReader(epub, {kSectionSpine, kPagesInSpine - 1, kPagesInSpine});

  EXPECT_FLOAT_EQ(ko.percentage, percentageInto(kSectionSpine, 1.0f));
  EXPECT_EQ(ko.xpath, "/body/DocFragment[2]/body/section[1]/p[60]");

  const auto back = ProgressMapper::toCrossPoint(epub, ko, kSectionSpine, kPagesInSpine);
  EXPECT_EQ(back.spineIndex, kSectionSpine);
  EXPECT_EQ(back.pageNumber, kPagesInSpine - 2);
}

TEST_F(ProgressMapperFixture, OnePageChapterUploadsAsItsStart) {
  const auto ko = ProgressMapper::toKOReader(epub, {kSectionSpine, 0, 1});

  EXPECT_FLOAT_EQ(ko.percentage, percentageInto(kSectionSpine, 0.0f));
}

TEST_F(ProgressMapperFixture, ChapterWithNoPageCountUploadsAsItsStart) {
  // 0/0 is the finished-book sentinel in progress.bin; the reader rewrites it before syncing, but
  // the mapper must not divide by it either way.
  const auto ko = ProgressMapper::toKOReader(epub, {kSectionSpine, 0, 0});

  EXPECT_FLOAT_EQ(ko.percentage, percentageInto(kSectionSpine, 0.0f));
}

// --- What the comparison sees -------------------------------------------------------------------

TEST_F(ProgressMapperFixture, ASpineFromTheXPathIsTrustedAndOneFromThePercentageIsNot) {
  EXPECT_TRUE(map("/body/DocFragment[2]/body/section/p[30]/text().0", percentageInto(kSectionSpine, 0.5f))
                  .hasResolvedSpineIndex);
  // Still trusted when the resolver fails: the spine is string-derived.
  EXPECT_TRUE(map("/body/DocFragment[3]/body/p[30]/text().0", percentageInto(kBrokenSpine, 0.4f), kBrokenSpine)
                  .hasResolvedSpineIndex);
  EXPECT_FALSE(map("", percentageInto(kFlatSpine, 0.5f)).hasResolvedSpineIndex);
  EXPECT_FALSE(map("/body/DocFragment[99]/body/p[1]", percentageInto(kFlatSpine, 0.5f)).hasResolvedSpineIndex);
}

TEST_F(ProgressMapperFixture, UnparseableChapterUploadsAsItsRoot) {
  // The forward mapper cannot count the chapter's text, so all it can say is "this chapter".
  const auto ko = ProgressMapper::toKOReader(epub, {kBrokenSpine, 20, kPagesInSpine});

  EXPECT_EQ(ko.xpath, "/body/DocFragment[3]/body");
  EXPECT_FLOAT_EQ(ko.percentage, percentageInto(kBrokenSpine, 20.0f / (kPagesInSpine - 1)));
}

// --- Push by content offset ---------------------------------------------------------------------
//
// With the chapter laid out, a page's start is a visible-text offset in the section cache. The
// push names the text at that offset to the character, in the shape KOReader itself emits.

// Lays `spine` out with the stub renderer and loads the section cache, LUTs included.
std::unique_ptr<Section> buildSection(const std::shared_ptr<Epub>& epub, const int spine, GfxRenderer& renderer) {
  Section::BuildParams p;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  auto section = std::make_unique<Section>(epub, spine, renderer);
  EXPECT_TRUE(section->createSectionFile(p, {}, true));
  EXPECT_TRUE(section->loadSectionFile(p));
  return section;
}

struct OffsetPushFixture : ProgressMapperFixture {
  GfxRenderer renderer;

  std::unique_ptr<Section> build(const int spine) { return buildSection(epub, spine, renderer); }

  CrossPointPosition pageWithOffset(const Section& section, const int spine, const int page) const {
    CrossPointPosition pos{};
    pos.spineIndex = spine;
    pos.pageNumber = page;
    pos.totalPages = section.pageCount;
    if (const auto off = section.getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
      pos.visibleTextOffset = *off;
      pos.hasVisibleTextOffset = true;
    }
    return pos;
  }
};

TEST_F(OffsetPushFixture, AMidParagraphPageStartPushesAsATextPoint) {
  const auto section = build(kSectionSpine);
  ASSERT_GT(section->pageCount, 3);
  // 60 equal paragraphs of four lines and more with the stub renderer: page 2 starts inside a
  // paragraph, not at its first word (page 1 happens to start at one). Shorter paragraphs would
  // not do: widow and orphan control never splits one of three lines or fewer.
  const auto pos = pageWithOffset(*section, kSectionSpine, 2);
  ASSERT_TRUE(pos.hasVisibleTextOffset);

  const auto ko = ProgressMapper::toKOReader(epub, pos);
  // "Target paragraph 20 carries ... on the page, and then a " is 97 codepoints: the page opens
  // on "second".
  EXPECT_EQ(ko.xpath, "/body/DocFragment[2]/body/section[1]/p[20]/text()[1].97");
}

TEST_F(OffsetPushFixture, APageStartingAtAParagraphPushesAsItsFirstCodepoint) {
  const auto section = build(kSectionSpine);
  ASSERT_GT(section->pageCount, 3);
  const auto pos = pageWithOffset(*section, kSectionSpine, 1);
  ASSERT_TRUE(pos.hasVisibleTextOffset);

  const auto ko = ProgressMapper::toKOReader(epub, pos);
  EXPECT_EQ(ko.xpath, "/body/DocFragment[2]/body/section[1]/p[11]/text()[1].0");
}

TEST_F(OffsetPushFixture, TheChapterStartPushesAsOffsetZeroOfItsFirstText) {
  const auto section = build(kSectionSpine);
  const auto pos = pageWithOffset(*section, kSectionSpine, 0);
  ASSERT_TRUE(pos.hasVisibleTextOffset);
  const auto ko = ProgressMapper::toKOReader(epub, pos);
  // The heading's text sits inside inline elements (span/a/span): a text point inside them.
  EXPECT_EQ(ko.xpath,
            "/body/DocFragment[2]/body/section[1]/header[1]/hgroup[1]/h1[1]/span[1]/a[1]/span[1]/text()[1].0");
}

TEST_F(OffsetPushFixture, WithoutAnOffsetThePushUsesTheFraction) {
  CrossPointPosition pos{};
  pos.spineIndex = kSectionSpine;
  pos.pageNumber = 20;
  pos.totalPages = 40;
  const auto ko = ProgressMapper::toKOReader(epub, pos);
  EXPECT_EQ(ko.xpath.rfind("/body/DocFragment[2]/body/section[1]/p[", 0), 0u) << ko.xpath;
  EXPECT_EQ(ko.xpath.find("/text()"), std::string::npos) << ko.xpath;
}

TEST_F(OffsetPushFixture, AParagraphAfterAnEntityPushesToTheExactCodepoint) {
  // p[1] ends in &mdash;: the section counts it as its expansion, and so must the push, or every
  // later page's text point would be off by the entity's length.
  const auto section = build(kFlatSpine);
  ASSERT_GT(section->pageCount, 1);
  const auto pos = pageWithOffset(*section, kFlatSpine, 1);
  ASSERT_TRUE(pos.hasVisibleTextOffset);
  const auto ko = ProgressMapper::toKOReader(epub, pos);
  // "Earlier paragraph 9 carries ... on the page, and then a " is 97 codepoints: the page opens
  // on "second".
  EXPECT_EQ(ko.xpath, "/body/DocFragment[1]/body/p[9]/text()[1].97");
}

// --- Pull by content offset ---------------------------------------------------------------------
//
// A text point matched exactly names a point in the chapter's visible text; the reader turns that
// offset into the page it falls on through the section's LUT. Nothing else carries an offset: an
// inexact match is only a stand-in for the element KOReader named, and an element matched exactly
// is anchored at its first direct text or, lacking any, at its END tag, which is no page start.

TEST_F(ProgressMapperFixture, AnExactMatchCarriesItsOffset) {
  const auto pos = map("/body/DocFragment[2]/body/section/p[30]/text().0", percentageInto(kSectionSpine, 0.5f));
  EXPECT_TRUE(pos.hasVisibleTextOffset);
  EXPECT_GT(pos.visibleTextOffset, 0u);
  // p[30] of 60 equal paragraphs: a little under half the chapter's text.
  const auto total = ChapterXPathIndexerInternal::countTotalTextBytes(epub, kSectionSpine);
  ASSERT_TRUE(total.has_value());
  EXPECT_NEAR(static_cast<double>(pos.visibleTextOffset) / static_cast<double>(*total), 0.48, 0.02);
}

TEST_F(ProgressMapperFixture, TheChapterHeadingCarriesOffsetZero) {
  const auto pos = map("/body/DocFragment[2]/body/section/header/hgroup/h1/span[1]/a/span/text().0",
                       percentageInto(kSectionSpine, 0.3f));
  EXPECT_TRUE(pos.hasVisibleTextOffset);
  EXPECT_EQ(pos.visibleTextOffset, 0u);
}

TEST_F(ProgressMapperFixture, AnInexactMatchCarriesNoOffset) {
  const std::string xpath = "/body/DocFragment[2]/body/section/div[4]/p[2]/text().0";
  // The chapter has no div: the resolver does match, but only through a stand-in.
  float intra = 0.0f;
  bool exact = true;
  ASSERT_TRUE(ChapterXPathIndexer::findProgressForXPath(epub, kSectionSpine, xpath, intra, exact));
  ASSERT_FALSE(exact);

  EXPECT_FALSE(map(xpath, percentageInto(kSectionSpine, 0.4f)).hasVisibleTextOffset);
}

TEST_F(ProgressMapperFixture, AFailedResolveCarriesNoOffset) {
  EXPECT_FALSE(map("/body/DocFragment[3]/body/p[30]/text().0", percentageInto(kBrokenSpine, 0.4f), kBrokenSpine)
                   .hasVisibleTextOffset);
}

TEST_F(OffsetPushFixture, AMidParagraphPageStartPullsBackToItsOffset) {
  const auto section = build(kSectionSpine);
  ASSERT_GT(section->pageCount, 3);
  const auto pos = pageWithOffset(*section, kSectionSpine, 2);  // see AMidParagraphPageStart...
  ASSERT_TRUE(pos.hasVisibleTextOffset);

  const auto ko = ProgressMapper::toKOReader(epub, pos);
  const auto back = ProgressMapper::toCrossPoint(epub, ko, kSectionSpine, section->pageCount);
  ASSERT_TRUE(back.hasVisibleTextOffset) << ko.xpath;
  EXPECT_EQ(back.visibleTextOffset, pos.visibleTextOffset) << ko.xpath;
  EXPECT_EQ(section->getPageForVisibleTextOffset(back.visibleTextOffset), std::optional<uint16_t>(2));
}

TEST_F(OffsetPushFixture, AParagraphAfterAnEntityPullsBackToItsOffset) {
  const auto section = build(kFlatSpine);
  ASSERT_GT(section->pageCount, 1);
  const auto pos = pageWithOffset(*section, kFlatSpine, 1);
  ASSERT_TRUE(pos.hasVisibleTextOffset);

  const auto ko = ProgressMapper::toKOReader(epub, pos);
  const auto back = ProgressMapper::toCrossPoint(epub, ko, kFlatSpine, section->pageCount);
  ASSERT_TRUE(back.hasVisibleTextOffset) << ko.xpath;
  EXPECT_EQ(back.visibleTextOffset, pos.visibleTextOffset) << ko.xpath;
  EXPECT_EQ(section->getPageForVisibleTextOffset(back.visibleTextOffset), std::optional<uint16_t>(1));
}

// Calibre's shape: every paragraph's text sits in a <span>, so a <p> has no direct text and its
// element match is anchored at its END tag.
struct ElementPullFixture : testing::Test {
  fs::path work;
  int bookCount = 0;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("element_pull_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::shared_ptr<Epub> book(const std::string& body) { return syntheticBook(work, bookCount++, body); }

  static CrossPointPosition pull(const std::shared_ptr<Epub>& epub, const std::string& xpath) {
    return ProgressMapper::toCrossPoint(epub, KOReaderPosition{xpath, 0.5f});
  }

  // The page the reader lands on: applyPendingSyncSession's order of preference (offset, list
  // item, paragraph, estimate), each resolved as NavigationTarget::resolveInto resolves it.
  static int landingPage(const CrossPointPosition& pos, const Section& section) {
    if (pos.hasVisibleTextOffset) {
      if (const auto p = section.getPageForVisibleTextOffset(pos.visibleTextOffset)) return *p;
      return pos.pageNumber;
    }
    if (pos.hasListItemIndex) {
      if (const auto p = section.getPageForListItemIndex(pos.listItemIndex)) return *p;
      if (const auto p = section.getPageForParagraphIndex(pos.listItemIndex)) return *p;
      return pos.pageNumber;
    }
    if (pos.hasParagraphIndex) {
      if (const auto p = section.getPageForParagraphIndex(pos.paragraphIndex)) return *p;
    }
    return pos.pageNumber;
  }
};

const char* const kSpanParagraphs = "<p><span>first paragraph text</span></p><p><span>second paragraph text</span></p>";

TEST_F(ElementPullFixture, AnElementMatchCarriesNoOffset) {
  const auto epub = book(kSpanParagraphs);

  const auto paragraph = pull(epub, "/body/DocFragment[1]/body/p[2]");
  EXPECT_EQ(paragraph.spineIndex, 0);
  EXPECT_FALSE(paragraph.hasVisibleTextOffset) << paragraph.visibleTextOffset;
  // The paragraph LUT lands it, as it did before offsets.
  EXPECT_TRUE(paragraph.hasParagraphIndex);
  EXPECT_EQ(paragraph.paragraphIndex, 2);

  // The chapter's root is matched at </body>, the chapter's last byte.
  EXPECT_FALSE(pull(epub, "/body/DocFragment[1]/body").hasVisibleTextOffset);
}

TEST_F(ElementPullFixture, ATextPointInsideTheSpanCarriesItsOffset) {
  const auto pos = pull(book(kSpanParagraphs), "/body/DocFragment[1]/body/p[2]/span[1]/text().3");
  ASSERT_TRUE(pos.hasVisibleTextOffset);
  // "firstparagraphtext" is 18 visible bytes, "sec" 3 more.
  EXPECT_EQ(pos.visibleTextOffset, 21u);
}

TEST_F(ElementPullFixture, AnElementPullOfALongParagraphLandsWhereItStarts) {
  std::string text;
  for (int i = 0; i < 80; ++i) text += "The second paragraph runs on with ordinary words for a while. ";
  const auto epub = book("<p><span>first paragraph text</span></p><p><span>" + text + "</span></p>");
  const auto section = buildSection(epub, 0, renderer);
  ASSERT_GT(section->pageCount, 1);

  // Landing on the element's anchor would be the paragraph's last page.
  EXPECT_EQ(landingPage(pull(epub, "/body/DocFragment[1]/body/p[2]"), *section), 0);
}

}  // namespace

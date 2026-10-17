// Which TOC a book gets when its EPUB 3 nav and its NCX disagree.
//
// The nav is preferred, and the NCX used to be read only when the nav failed to parse. Converted
// books (Amazon exports among them) pair a nav listing only parts and front matter with an NCX
// carrying every chapter, so they opened with a handful of entries. A parsed nav whose entries map
// to less than half the spine now gives the NCX a chance, and the source with more spine-mapped
// entries wins. The policy cases mirror crosspoint-reader PR #2603 (Ryan Jarvis, @Cabalist).
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/TocFallbackPolicy.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

using toc_fallback::navIsSparse;
using toc_fallback::shouldAdoptNcx;

struct Link {
  std::string href;  // as written in the nav or NCX
  std::string title;
};

// The book's layout: OEBPS/content.opf, chapters under OEBPS/text/, the nav at OEBPS/nav.xhtml and
// the NCX at OEBPS/toc.ncx. Spine files are given relative to OEBPS/; links relative to the
// document that holds them (both sit in OEBPS/).
struct BookShape {
  std::vector<std::string> spine;
  std::vector<Link> nav;
  bool hasNcx = false;
  std::vector<Link> ncx;
  std::vector<Link> navPageList;
};

std::vector<std::string> chapters(const int count) {
  std::vector<std::string> out;
  for (int i = 0; i < count; ++i) out.push_back("text/c" + std::to_string(i) + ".xhtml");
  return out;
}

std::vector<Link> links(const std::vector<int>& chapterIndices, const std::string& titlePrefix,
                        const std::string& dir = "text/") {
  std::vector<Link> out;
  for (const int i : chapterIndices) {
    out.push_back({dir + "c" + std::to_string(i) + ".xhtml", titlePrefix + " " + std::to_string(i)});
  }
  return out;
}

class TocSource : public testing::Test {
 protected:
  fs::path work;
  std::string bookPath;
  std::string cacheDir;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("toc_source_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    bookPath = (work / "book.epub").string();
    cacheDir = (work / "cache").string();
  }
  void TearDown() override { fs::remove_all(work); }

  void writeBook(const BookShape& shape) const {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
            "</container>\n");
    std::string manifest;
    std::string spine;
    for (size_t i = 0; i < shape.spine.size(); ++i) {
      const std::string id = "s" + std::to_string(i);
      manifest += "<item id=\"" + id + "\" href=\"" + shape.spine[i] + "\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"" + id + "\"/>\n";
      zip.add("OEBPS/" + shape.spine[i],
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
              "<head><title>C</title></head><body><p>Spine item " +
                  std::to_string(i) + ".</p></body></html>\n");
    }

    manifest += "<item id=\"nav\" href=\"nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\"/>\n";
    std::string navBody = "<nav epub:type=\"toc\"><ol>\n";
    for (const Link& l : shape.nav) navBody += "<li><a href=\"" + l.href + "\">" + l.title + "</a></li>\n";
    navBody += "</ol></nav>\n";
    if (!shape.navPageList.empty()) {
      navBody += "<nav epub:type=\"page-list\"><ol>\n";
      for (const Link& l : shape.navPageList) navBody += "<li><a href=\"" + l.href + "\">" + l.title + "</a></li>\n";
      navBody += "</ol></nav>\n";
    }
    zip.add("OEBPS/nav.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
            "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Contents</title></head><body>\n" +
                navBody + "</body></html>\n");

    if (shape.hasNcx) {
      manifest += "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
      std::string navPoints;
      for (size_t i = 0; i < shape.ncx.size(); ++i) {
        navPoints += "<navPoint id=\"n" + std::to_string(i) + "\"><navLabel><text>" + shape.ncx[i].title +
                     "</text></navLabel><content src=\"" + shape.ncx[i].href + "\"/></navPoint>\n";
      }
      zip.add("OEBPS/toc.ncx",
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" "
              "version=\"2005-1\">\n<navMap>\n" +
                  navPoints + "</navMap>\n</ncx>\n");
    }

    zip.add("OEBPS/content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">toc-source</dc:identifier>"
            "<dc:title>Toc Source</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n" +
                manifest + "</manifest>\n<spine" + (shape.hasNcx ? " toc=\"ncx\"" : "") + ">\n" + spine +
                "</spine>\n</package>\n");
    fs::remove(bookPath);
    zip.write(bookPath);
  }

  std::unique_ptr<Epub> open() const {
    auto epub = std::make_unique<Epub>(bookPath, cacheDir);
    EXPECT_TRUE(epub->load(true, true));
    return epub;
  }

  static std::vector<std::string> titles(Epub& epub) {
    std::vector<std::string> out;
    for (int i = 0; i < epub.getTocItemsCount(); ++i) out.push_back(epub.getTocItem(i).title);
    return out;
  }
  // As stored: getSpineIndexForTocIndex() reports an unmapped entry as spine 0.
  static std::vector<int> spineIndices(const Epub& epub) {
    std::vector<int> out;
    for (int i = 0; i < epub.getTocItemsCount(); ++i) out.push_back(epub.getTocItem(i).spineIndex);
    return out;
  }
};

}  // namespace

// --- The decision table ------------------------------------------------------------------------

TEST(TocFallbackPolicy, AnUnparsedNavIsNeverSparse) {
  EXPECT_FALSE(navIsSparse(false, 0, 20));
  EXPECT_FALSE(navIsSparse(false, 5, 20));
}

TEST(TocFallbackPolicy, ANavMappingUnderHalfTheSpineIsSparse) {
  EXPECT_TRUE(navIsSparse(true, 5, 11));
  EXPECT_TRUE(navIsSparse(true, 0, 8));
  EXPECT_FALSE(navIsSparse(true, 5, 10));
  EXPECT_FALSE(navIsSparse(true, 0, 0));
}

TEST(TocFallbackPolicy, TheNcxMustMapStrictlyMoreToReplaceAParsedNav) {
  EXPECT_TRUE(shouldAdoptNcx(true, 2, true, 12));
  EXPECT_FALSE(shouldAdoptNcx(true, 5, true, 5));
  EXPECT_FALSE(shouldAdoptNcx(true, 5, true, 4));
  EXPECT_FALSE(shouldAdoptNcx(true, 5, true, 0)) << "a stale NCX must not replace a working nav";
  EXPECT_FALSE(shouldAdoptNcx(true, 5, false, 0));
  EXPECT_FALSE(shouldAdoptNcx(true, 0, true, 0));
}

TEST(TocFallbackPolicy, WithoutANavAnyParsedNcxIsTaken) {
  EXPECT_TRUE(shouldAdoptNcx(false, 0, true, 0));
  EXPECT_FALSE(shouldAdoptNcx(false, 0, false, 0));
}

// --- Whole books --------------------------------------------------------------------------------

TEST_F(TocSource, ASparseNavGivesWayToAnNcxListingEveryChapter) {
  BookShape shape;
  shape.spine = chapters(8);
  shape.nav = links({0, 4}, "Part");
  shape.hasNcx = true;
  shape.ncx = links({0, 1, 2, 3, 4, 5, 6, 7}, "Chapter");
  writeBook(shape);

  const auto epub = open();
  ASSERT_EQ(epub->getTocItemsCount(), 8);
  EXPECT_EQ(epub->getTocItem(0).title, "Chapter 0");
  EXPECT_EQ(spineIndices(*epub), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST_F(TocSource, ASparseNavStaysWhenTheNcxMapsNoMore) {
  BookShape shape;
  shape.spine = chapters(8);
  shape.nav = links({0, 4}, "Nav");
  shape.hasNcx = true;
  shape.ncx = links({0, 4}, "Ncx");
  writeBook(shape);

  const auto epub = open();
  EXPECT_EQ(titles(*epub), (std::vector<std::string>{"Nav 0", "Nav 4"})) << "a tie keeps the nav, entries not doubled";
  EXPECT_EQ(spineIndices(*epub), (std::vector<int>{0, 4}));
}

TEST_F(TocSource, AStaleNcxDoesNotReplaceAWorkingNav) {
  BookShape shape;
  shape.spine = chapters(8);
  shape.nav = links({0, 4}, "Nav");
  shape.hasNcx = true;
  for (int i = 0; i < 8; ++i) {  // files the book does not have: nothing maps
    shape.ncx.push_back({"old/gone" + std::to_string(i) + ".xhtml", "Gone " + std::to_string(i)});
  }
  writeBook(shape);

  const auto epub = open();
  EXPECT_EQ(titles(*epub), (std::vector<std::string>{"Nav 0", "Nav 4"}));
}

TEST_F(TocSource, ANavCoveringHalfTheSpineIsKept) {
  BookShape shape;
  shape.spine = chapters(8);
  shape.nav = links({0, 2, 4, 6}, "Nav");
  shape.hasNcx = true;
  shape.ncx = links({0, 1, 2, 3, 4, 5, 6, 7}, "Ncx");
  writeBook(shape);

  const auto epub = open();
  EXPECT_EQ(epub->getTocItemsCount(), 4);
  EXPECT_EQ(epub->getTocItem(0).title, "Nav 0");
}

TEST_F(TocSource, AnEmptyNavGivesWayToTheNcx) {
  BookShape shape;
  shape.spine = chapters(4);
  shape.hasNcx = true;
  shape.ncx = links({0, 1, 2, 3}, "Ncx");
  writeBook(shape);

  EXPECT_EQ(open()->getTocItemsCount(), 4);
}

TEST_F(TocSource, TheNavsPageListOutlivesAnAdoptedNcx) {
  BookShape shape;
  shape.spine = chapters(8);
  shape.nav = links({0, 4}, "Part");
  shape.navPageList = {{"text/c1.xhtml", "11"}, {"text/c5.xhtml", "12"}};
  shape.hasNcx = true;  // with no <pageList> of its own
  shape.ncx = links({0, 1, 2, 3, 4, 5, 6, 7}, "Chapter");
  writeBook(shape);

  const auto epub = open();
  EXPECT_EQ(epub->getTocItemsCount(), 8);
  const auto page = epub->findPrintedPageByLabel(12);
  ASSERT_TRUE(page.has_value()) << "the nav's printed-page list was dropped by the NCX parse";
  EXPECT_EQ(page->href, "OEBPS/text/c5.xhtml");
}

// --- Nav links relative to another folder -------------------------------------------------------
//
// The nav sits in OEBPS/ but links as if it sat beside the chapters in OEBPS/text/. Each link
// resolves to OEBPS/cN.xhtml, which no spine item matches exactly: the contents list showed every
// title and opened none. Ported from crosspoint-reader PR #2987 (Hoang Manh Linh, @jetaudio).

TEST_F(TocSource, ANavLinkingFromTheWrongFolderStillOpensItsChapters) {
  BookShape shape;
  shape.spine = chapters(8);
  shape.nav = links({0, 1, 2, 3, 4, 5, 6, 7}, "Chapter", /*dir=*/"");
  writeBook(shape);

  const auto epub = open();
  EXPECT_EQ(spineIndices(*epub), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST_F(TocSource, AFilenameTwoFoldersShareIsNotGuessed) {
  BookShape shape;
  shape.spine = chapters(4);
  shape.spine.push_back("a/notes.xhtml");      // 4
  shape.spine.push_back("extra/notes.xhtml");  // 5
  shape.nav = {{"text/c0.xhtml", "One"}, {"notes.xhtml", "Notes?"}, {"extra/notes.xhtml", "Notes"}};
  writeBook(shape);

  const auto epub = open();
  EXPECT_EQ(spineIndices(*epub), (std::vector<int>{0, -1, 5}));
}

// In-book links resolve through the same filename match, unchanged: the first spine item that
// matches exactly or by filename.
TEST_F(TocSource, InBookLinksStillResolveByFilename) {
  BookShape shape;
  shape.spine = chapters(4);
  shape.nav = links({0, 1, 2, 3}, "Chapter");
  writeBook(shape);

  const auto epub = open();
  EXPECT_EQ(epub->resolveHrefToSpineIndex("c2.xhtml#note"), 2);
  EXPECT_EQ(epub->resolveHrefToSpineIndex("OEBPS/text/c3.xhtml"), 3);
  EXPECT_EQ(epub->resolveHrefToSpineIndex("nowhere.xhtml"), -1);
  EXPECT_EQ(epub->resolveHrefToSpineIndex("#same-file"), -1);
}

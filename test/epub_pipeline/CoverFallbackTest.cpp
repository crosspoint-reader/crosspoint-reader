// Cover fallbacks for books that name no cover picture in their manifest, or name one the reader
// cannot use: each case is a book shape and the picture a reader should show for it. Most began as
// the probes of a review of witchling-reader commit 5b62bbee (syfq91, "improve cover resolution
// using TOC, wrappers, and prioritized fallback"), the shapes that commit set out to fix; before
// the fixes here, five of them showed a title banner, a logo or no cover at all.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"
#include "Epub/NavCoverScanner.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

// What cover generation sniffs for: enough of a PNG for ImageFormatDetector to call it one.
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "not-decoded-here";
const std::string kBigPng = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(4096, 'x');

struct Book {
  std::string metadataExtra;  // extra <metadata> children, e.g. <meta name="cover" .../>
  std::string manifestExtra;  // extra <item>s
  std::string guide;          // whole <guide> element or empty
  std::string version = "3.0";
  std::vector<std::pair<std::string, std::string>> files;  // zip path -> contents, in CD order
};

class CoverFallback : public testing::Test {
 protected:
  fs::path work;
  std::string bookPath;
  std::string cacheDir;

  void SetUp() override {
    // Per process as well as per test: other checkouts may run this suite at the same time.
    work = fs::temp_directory_path() / ("cover_fallback_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    bookPath = (work / "book.epub").string();
    cacheDir = (work / "cache").string();
    Epub::clearCoverMetadataMemo();
  }
  void TearDown() override {
    Epub::clearCoverMetadataMemo();
    fs::remove_all(work);
  }

  void writeBook(const Book& book) const {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?><container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
            "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>"
            "</container>");
    const std::string manifest =
        "<item media-type=\"application/xhtml+xml\" id=\"p-001\" href=\"xhtml/p-001.xhtml\"/>\n" + book.manifestExtra;
    zip.add("OEBPS/content.opf",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"" +
                book.version +
                "\" unique-identifier=\"id\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                "<dc:identifier id=\"id\">cover-fallback</dc:identifier><dc:title>Cover Fallback</dc:title>"
                "<dc:language>en</dc:language>" +
                book.metadataExtra + "</metadata>\n<manifest>\n" + manifest +
                "</manifest>\n<spine><itemref idref=\"p-001\"/></spine>\n" + book.guide + "</package>\n");
    zip.add("OEBPS/xhtml/p-001.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
            "<title>P</title></head><body><p>Text.</p></body></html>\n");
    for (const auto& [name, data] : book.files) zip.add(name, data);
    fs::remove(bookPath);
    zip.write(bookPath);
  }

  // As the reader opens a book (book.bin carries the cover href) and as the home screen asks for a
  // cover without building book.bin.
  std::string coverViaLoad() const {
    Epub epub(bookPath, cacheDir);
    EXPECT_TRUE(epub.load(true, true));
    return epub.getCoverItemHref();
  }
  std::string coverViaLoadForCover() const {
    Epub::clearCoverMetadataMemo();
    Epub epub(bookPath, (work / "cache_cover_only").string());
    EXPECT_TRUE(epub.loadForCover());
    return epub.getCoverItemHref();
  }
};

std::string xhtmlWithImg(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
         "<title>C</title></head><body>" +
         body + "</body></html>\n";
}

std::string navDocument(const std::string& tocItems, const std::string& landmarkItems) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
         "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>N</title></head><body>"
         "<nav epub:type=\"toc\"><ol>" +
         tocItems + "</ol></nav>" +
         (landmarkItems.empty() ? "" : "<nav epub:type=\"landmarks\"><ol>" + landmarkItems + "</ol></nav>") +
         "</body></html>\n";
}

}  // namespace

// The ZIP-name scan took the first name it knew in archive order, and "title" was one of them, so a
// title banner stored ahead of cover.png became the cover.
TEST_F(CoverFallback, ZipScanPrefersCoverOverAnEarlierTitleBanner) {
  Book book;
  book.manifestExtra =
      "<item id=\"i1\" media-type=\"image/png\" href=\"image/title.png\"/>\n"
      "<item id=\"i2\" media-type=\"image/png\" href=\"image/cover.png\"/>\n";
  book.files = {{"OEBPS/image/title.png", kPng}, {"OEBPS/image/cover.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/cover.png");
  EXPECT_EQ(coverViaLoadForCover(), "OEBPS/image/cover.png");
}

TEST_F(CoverFallback, ZipScanRanksAVariantOfCoverAboveATitlePage) {
  Book book;
  book.manifestExtra =
      "<item id=\"i1\" media-type=\"image/png\" href=\"image/titlepage.png\"/>\n"
      "<item id=\"i2\" media-type=\"image/png\" href=\"image/back_cover.png\"/>\n"
      "<item id=\"i3\" media-type=\"image/png\" href=\"image/9780000000000_cover.png\"/>\n";
  book.files = {{"OEBPS/image/titlepage.png", kPng},
                {"OEBPS/image/back_cover.png", kPng},
                {"OEBPS/image/9780000000000_cover.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/9780000000000_cover.png");
}

TEST_F(CoverFallback, ZipScanStillTakesATitlePageWhenNothingSaysCover) {
  Book book;
  book.manifestExtra = "<item id=\"i1\" media-type=\"image/png\" href=\"image/TitlePage.png\"/>\n";
  book.files = {{"OEBPS/image/title.png", kPng}, {"OEBPS/image/TitlePage.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/TitlePage.png");
}

// <meta name="cover"> naming an XHTML page was dropped. The picture inside has a name the ZIP scan
// does not know.
TEST_F(CoverFallback, MetaCoverNamingAnXhtmlWrapperLeadsToItsPicture) {
  Book book;
  book.version = "2.0";
  book.metadataExtra = "<meta name=\"cover\" content=\"coverpage\"/>";
  book.manifestExtra =
      "<item id=\"coverpage\" media-type=\"application/xhtml+xml\" href=\"xhtml/cover.xhtml\"/>\n"
      "<item id=\"img\" media-type=\"image/png\" href=\"image/front.png\"/>\n";
  book.files = {{"OEBPS/xhtml/cover.xhtml", xhtmlWithImg("<img src=\"../image/front.png\" alt=\"\"/>")},
                {"OEBPS/image/front.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/front.png");
  EXPECT_EQ(coverViaLoadForCover(), "OEBPS/image/front.png");
}

// A manifest image whose id says cover, without properties or meta, was not found when its file
// name means nothing.
TEST_F(CoverFallback, ManifestItemIdCoverImageIsUsed) {
  Book book;
  book.version = "2.0";
  book.manifestExtra = "<item id=\"cover-image\" media-type=\"image/png\" href=\"image/img0001.png\"/>\n";
  book.files = {{"OEBPS/image/img0001.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/img0001.png");
  EXPECT_EQ(coverViaLoadForCover(), "OEBPS/image/img0001.png");
}

// The fork took any image/* item with a cover id, SVG included, and cover generation cannot decode
// an SVG (the bug ff57c2e8 fixed). An SVG with a cover id is a wrapper to look inside.
TEST_F(CoverFallback, ManifestItemIdSvgIsAWrapperNotThePicture) {
  Book book;
  book.version = "2.0";
  book.manifestExtra =
      "<item id=\"Cover\" media-type=\"image/svg+xml\" href=\"image/c.svg\"/>\n"
      "<item id=\"img\" media-type=\"image/png\" href=\"image/img0002.png\"/>\n";
  book.files = {{"OEBPS/image/c.svg",
                 "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
                 "<image xlink:href=\"img0002.png\"/></svg>"},
                {"OEBPS/image/img0002.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/img0002.png");
}

// An EPUB 3 nav landmarks entry epub:type="cover" was not followed.
TEST_F(CoverFallback, NavLandmarksCoverIsFollowed) {
  Book book;
  book.manifestExtra =
      "<item id=\"nav\" properties=\"nav\" media-type=\"application/xhtml+xml\" href=\"nav.xhtml\"/>\n"
      "<item id=\"cpage\" media-type=\"application/xhtml+xml\" href=\"xhtml/c.xhtml\"/>\n"
      "<item id=\"img\" media-type=\"image/png\" href=\"image/front.png\"/>\n";
  book.files = {
      {"OEBPS/nav.xhtml", navDocument("<li><a href=\"xhtml/p-001.xhtml\">One</a></li>",
                                      "<li><a epub:type=\"cover\" href=\"xhtml/c.xhtml#top\">Cover</a></li>")},
      {"OEBPS/xhtml/c.xhtml", xhtmlWithImg("<img src=\"../image/front.png\" alt=\"\"/>")},
      {"OEBPS/image/front.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/front.png");
  EXPECT_EQ(coverViaLoadForCover(), "OEBPS/image/front.png");
}

TEST_F(CoverFallback, ANavLandmarkMayNameThePictureItself) {
  Book book;
  book.manifestExtra =
      "<item id=\"nav\" properties=\"nav\" media-type=\"application/xhtml+xml\" href=\"nav/toc.xhtml\"/>\n"
      "<item id=\"img\" media-type=\"image/png\" href=\"image/front.png\"/>\n";
  book.files = {
      {"OEBPS/nav/toc.xhtml", navDocument("<li><a href=\"../xhtml/p-001.xhtml\">One</a></li>",
                                          "<li><a href=\"../image/front.png\" epub:type=\"cover\">Cover</a></li>")},
      {"OEBPS/image/front.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/front.png");
}

// properties tokens separated by a newline or a tab: already recognised (the SAX parser normalises
// attribute whitespace), kept as tests because the fork claimed them broken.
TEST_F(CoverFallback, CoverImagePropertySeparatedByNewline) {
  Book book;
  book.manifestExtra =
      "<item id=\"x\" media-type=\"image/png\" href=\"image/img0001.png\" properties=\"svg\ncover-image\"/>\n";
  book.files = {{"OEBPS/image/img0001.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/img0001.png");
}

TEST_F(CoverFallback, CoverImagePropertySeparatedByTab) {
  Book book;
  book.manifestExtra =
      "<item id=\"x\" media-type=\"image/png\" href=\"image/img0001.png\" properties=\"svg\tcover-image\"/>\n";
  book.files = {{"OEBPS/image/img0001.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/img0001.png");
}

// A guide cover page that shows a small publisher logo before the cover picture took the logo: the
// first reference won. Of several pictures of one kind, the largest file is the cover.
TEST_F(CoverFallback, GuideCoverPageWithLogoBeforeCoverPicture) {
  Book book;
  book.version = "2.0";
  book.manifestExtra =
      "<item id=\"cpage\" media-type=\"application/xhtml+xml\" href=\"xhtml/c.xhtml\"/>\n"
      "<item id=\"logo\" media-type=\"image/png\" href=\"image/logo.png\"/>\n"
      "<item id=\"art\" media-type=\"image/png\" href=\"image/art.png\"/>\n";
  book.guide = "<guide><reference type=\"cover\" href=\"xhtml/c.xhtml\"/></guide>\n";
  book.files = {{"OEBPS/xhtml/c.xhtml",
                 xhtmlWithImg("<img src=\"../image/logo.png\" alt=\"\"/><img src=\"../image/art.png\" alt=\"\"/>")},
                {"OEBPS/image/logo.png", kPng},
                {"OEBPS/image/art.png", kBigPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/art.png");
}

// The fork's contents rule takes an entry labelled "Title Page" ahead of the ZIP scan, so the title
// banner on that page beat a file named cover.png. No such rule here.
TEST_F(CoverFallback, TitlePageTocEntryDoesNotBeatACoverFile) {
  Book book;
  book.manifestExtra =
      "<item id=\"nav\" properties=\"nav\" media-type=\"application/xhtml+xml\" href=\"nav.xhtml\"/>\n"
      "<item id=\"tp\" media-type=\"application/xhtml+xml\" href=\"xhtml/titlepage.xhtml\"/>\n"
      "<item id=\"banner\" media-type=\"image/png\" href=\"image/banner.png\"/>\n"
      "<item id=\"img\" media-type=\"image/png\" href=\"image/cover.png\"/>\n";
  book.files = {{"OEBPS/nav.xhtml", navDocument("<li><a href=\"xhtml/titlepage.xhtml\">Title Page</a></li>"
                                                "<li><a href=\"xhtml/p-001.xhtml\">One</a></li>",
                                                "")},
                {"OEBPS/xhtml/titlepage.xhtml", xhtmlWithImg("<img src=\"../image/banner.png\" alt=\"\"/>")},
                {"OEBPS/image/banner.png", kPng},
                {"OEBPS/image/cover.png", kPng}};
  writeBook(book);
  EXPECT_EQ(coverViaLoad(), "OEBPS/image/cover.png");
}

// --- The landmark scanner on its own ------------------------------------------------------------

namespace {
std::string scanNav(const std::string& doc, const bool byteAtATime = false) {
  NavCoverScanner scanner;
  if (byteAtATime) {
    for (const char c : doc) scanner.write(static_cast<uint8_t>(c));
  } else {
    scanner.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size());
  }
  return std::string(scanner.result());
}
}  // namespace

TEST(NavCoverScanner, FindsTheCoverLinkWhateverTheAttributeOrder) {
  EXPECT_EQ(scanNav("<a epub:type=\"cover\" href=\"c.xhtml\">Cover</a>"), "c.xhtml");
  EXPECT_EQ(scanNav("<a href='c.xhtml#x' epub:type='frontmatter cover'>Cover</a>"), "c.xhtml#x");
  EXPECT_EQ(scanNav("<nav epub:type=\"landmarks\"><ol><li><a epub:type=\"toc\" href=\"t.xhtml\">T</a></li>"
                    "<li><a epub:type=\"cover\" href=\"c.xhtml\">C</a></li></ol></nav>",
                    true),
            "c.xhtml");
}

TEST(NavCoverScanner, IgnoresWhatIsNotACoverLink) {
  EXPECT_EQ(scanNav("<a epub:type=\"covers\" href=\"c.xhtml\">C</a>"), "");
  EXPECT_EQ(scanNav("<a epub:type=\"cover\">no href</a><a href=\"next.xhtml\">Next</a>"), "");
  EXPECT_EQ(scanNav("<p>A \"cover\" in text, epub:type=\"cover\" href=\"x.xhtml\"</p>"), "");
}

TEST(NavCoverScanner, AnApostropheInACommentDoesNotHideTheLandmarks) {
  EXPECT_EQ(scanNav("<!-- the reader's landmarks --><a epub:type=\"cover\" href=\"c.xhtml\">C</a>"), "c.xhtml");
}

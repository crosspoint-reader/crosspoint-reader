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
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

// What cover generation sniffs for: enough of a PNG for ImageFormatDetector to call it one.
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "not-decoded-here";

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

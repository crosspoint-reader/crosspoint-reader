// Books whose manifest names an SVG wrapper as the cover image, rather than the picture:
//
//   <item properties="cover-image" media-type="image/svg+xml" href="image/cover.svg"/>
//   cover.svg: <svg viewBox="0 0 600 900"><image xlink:href="cover.jpg"/></svg>
//
// The wrapper exists in the archive, so neither the guide scan nor the ZIP-name scan ran, and
// cover generation then refused the extracted SVG as an unsupported format: no cover on the home
// screen or the sleep screen. The shape is the one crosspoint-reader PR #3539 (OSAKANA TARO,
// @osakanataro) found in 56 of 1,941 commercial Japanese EPUBs.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Epub.h"
#include "Epub/CoverImageRefScanner.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

// What cover generation sniffs for: enough of a PNG for ImageFormatDetector to call it one.
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "not-decoded-here";

std::string svgWrapper(const std::string& imageAttr) {
  return "<?xml version='1.0' encoding='UTF-8'?>\n"
         "<svg viewBox=\"0 0 600 900\" version=\"1.1\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
         "xmlns=\"http://www.w3.org/2000/svg\">\n"
         "<image height=\"900\" width=\"600\" y=\"0\" x=\"0\" " +
         imageAttr + "></image></svg>\n";
}

struct Book {
  std::string coverItem;  // manifest href of the item named cover-image, relative to item/; empty: none
  std::string coverItemType = "image/svg+xml";
  std::string guideCover;                                  // guide cover page href relative to item/; empty: no guide
  std::vector<std::pair<std::string, std::string>> files;  // zip path -> contents
};

class CoverWrapper : public testing::Test {
 protected:
  fs::path work;
  std::string bookPath;
  std::string cacheDir;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("cover_wrapper_") + testing::UnitTest::GetInstance()->current_test_info()->name());
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

  // The OPF sits at item/standard.opf, as in the books that ship this shape.
  void writeBook(const Book& book) const {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?><container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
            "full-path=\"item/standard.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>"
            "</container>");
    std::string manifest = "<item media-type=\"application/xhtml+xml\" id=\"p-001\" href=\"xhtml/p-001.xhtml\"/>\n";
    if (!book.coverItem.empty()) {
      manifest += "<item media-type=\"" + book.coverItemType + "\" id=\"cover\" href=\"" + book.coverItem +
                  "\" properties=\"cover-image\"/>\n";
    }
    const std::string guide =
        book.guideCover.empty() ? "" : "<guide><reference type=\"cover\" href=\"" + book.guideCover + "\"/></guide>\n";
    zip.add("item/standard.opf",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"id\">cover-wrapper</dc:identifier><dc:title>Cover Wrapper</dc:title>"
            "<dc:language>en</dc:language></metadata>\n<manifest>\n" +
                manifest + "</manifest>\n<spine><itemref idref=\"p-001\"/></spine>\n" + guide + "</package>\n");
    zip.add("item/xhtml/p-001.xhtml",
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

TEST_F(CoverWrapper, AnSvgWrapperNamedAsTheCoverLeadsToItsPicture) {
  Book book;
  book.coverItem = "image/cover.svg";
  book.files = {{"item/image/cover.svg", svgWrapper("xlink:href=\"cover.png\"")}, {"item/image/cover.png", kPng}};
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "item/image/cover.png");
  EXPECT_EQ(coverViaLoadForCover(), "item/image/cover.png");

  Epub epub(bookPath, cacheDir);
  ASSERT_TRUE(epub.load(true, true));
  EXPECT_TRUE(epub.coverImageCachedAndValid(true)) << "the extracted cover must be a format the generators decode";
}

TEST_F(CoverWrapper, TheWrappersReferenceResolvesAgainstItsOwnFolder) {
  Book book;
  book.coverItem = "image/wrap/cover.svg";
  book.files = {{"item/image/wrap/cover.svg", svgWrapper("xlink:href=\"../pics/front%20cover.png\"")},
                {"item/image/pics/front cover.png", kPng}};
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "item/image/pics/front cover.png");
}

TEST_F(CoverWrapper, AnSvg2HrefIsFollowedToo) {
  Book book;
  book.coverItem = "image/cover.svg";
  book.files = {{"item/image/cover.svg", svgWrapper("href=\"cover.png\"")}, {"item/image/cover.png", kPng}};
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "item/image/cover.png");
}

TEST_F(CoverWrapper, AWrapperWithoutAPictureFallsBackToTheGuide) {
  Book book;
  book.coverItem = "image/cover.svg";
  book.guideCover = "xhtml/cover.xhtml";
  book.files = {{"item/image/cover.svg", svgWrapper("xlink:href=\"ornament.gif\"")},
                {"item/xhtml/cover.xhtml",
                 "<html><body><img src=\"../image/ornament.gif\"/><img src=\"../image/front.png\"/></body></html>"},
                {"item/image/front.png", kPng}};
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "item/image/front.png");
}

TEST_F(CoverWrapper, AWrapperWithoutAPictureFallsBackToTheZipNameScan) {
  Book book;
  book.coverItem = "image/cover.svg";
  book.files = {{"item/image/cover.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect/></svg>"},
                {"item/image/cover.png", kPng}};
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "item/image/cover.png");
}

TEST_F(CoverWrapper, APictureNamedAsTheCoverIsTakenAsItIs) {
  Book book;
  book.coverItem = "image/art.png";
  book.coverItemType = "image/png";
  book.files = {{"item/image/art.png", kPng}};
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "item/image/art.png");
}

TEST_F(CoverWrapper, TheGuideCoverPageStillPrefersItsSvgImage) {
  Book book;
  book.guideCover = "xhtml/cover.xhtml";
  book.files = {{"item/xhtml/cover.xhtml",
                 "<html><body><img src=\"../image/small.png\"/>"
                 "<svg><image xlink:href=\"../image/big.jpg\"/></svg></body></html>"},
                {"item/image/small.png", kPng},
                {"item/image/big.jpg", "\xFF\xD8\xFF"}};
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "item/image/big.jpg");
}

// --- A named cover missing from the archive -------------------------------------------------------
//
// The ZIP-name scan ran, but took the dead href still set for a cover already found and never
// looked, so the book kept a cover path nothing could extract.

TEST_F(CoverWrapper, ANamedCoverMissingFromTheArchiveFallsBackToTheZipNameScan) {
  Book book;
  book.coverItem = "image/missing.png";
  book.coverItemType = "image/png";
  book.files = {{"item/image/cover.png", kPng}};
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "item/image/cover.png");
}

TEST_F(CoverWrapper, ANamedCoverMissingFromTheArchiveLeavesNoDeadHref) {
  Book book;
  book.coverItem = "image/missing.png";
  book.coverItemType = "image/png";
  writeBook(book);

  EXPECT_EQ(coverViaLoad(), "") << "a book without a cover is known to have none, not one that fails to extract";
}

// --- The scanner on its own ---------------------------------------------------------------------

namespace {
std::string scan(const std::string& doc, const bool byteAtATime = false) {
  CoverImageRefScanner scanner;
  if (byteAtATime) {
    for (const char c : doc) scanner.write(static_cast<uint8_t>(c));
  } else {
    scanner.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size());
  }
  return std::string(scanner.result());
}
}  // namespace

TEST(CoverImageRefScanner, PrefersXlinkHrefThenSrcThenPlainHref) {
  EXPECT_EQ(scan("<img src=\"a.png\"/><image xlink:href=\"b.jpg\"/>"), "b.jpg");
  EXPECT_EQ(scan("<image href=\"c.png\"/><img src=\"a.png\"/>"), "a.png");
  EXPECT_EQ(scan("<image href=\"c.png\"/>"), "c.png");
  EXPECT_EQ(scan("<image ns0:href='d.jpeg'/>"), "d.jpeg") << "any prefix, either quote";
}

TEST(CoverImageRefScanner, PassesOverWhatCoverGenerationCannotDecode) {
  EXPECT_EQ(scan("<image xlink:href=\"orn.gif\"/><image xlink:href=\"pic.png\"/>"), "pic.png");
  EXPECT_EQ(scan("<link href=\"style.css\"/><a href=\"next.xhtml\">x</a>"), "");
  EXPECT_EQ(scan("<img src=\"data:image/png;base64,AAAA\"/>"), "");
}

TEST(CoverImageRefScanner, FindsAReferenceSplitAcrossWrites) {
  EXPECT_EQ(scan("<svg><image width=\"1\" xlink:href=\"../Images/cover.jpg\"/></svg>", true), "../Images/cover.jpg");
}

TEST(CoverImageRefScanner, PassesOverAValueTooLongToHold) {
  const std::string longName(CoverImageRefScanner::MAX_REF + 1, 'x');
  EXPECT_EQ(scan("<img src=\"" + longName + ".png\"/><img src=\"ok.png\"/>"), "ok.png");
}

TEST(CoverImageRefScanner, StopsReadingAfterItsByteBudget) {
  const std::string filler(CoverImageRefScanner::MAX_SCAN_BYTES, ' ');
  CoverImageRefScanner scanner;
  const std::string doc = filler + "<img src=\"late.png\"/>";
  scanner.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size());
  EXPECT_TRUE(*scanner.stopFlag());
  EXPECT_EQ(scanner.result(), "");
}

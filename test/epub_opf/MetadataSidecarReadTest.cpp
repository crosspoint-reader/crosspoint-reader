// MetadataSidecar: the .opf beside a book, read for any format (the Library labels TXT, Markdown and
// XTC books with it too), and the rule that keeps an author and its filing name together.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../lib/Epub/Epub/MetadataSidecar.h"

namespace fs = std::filesystem;

namespace opf_test_hooks {
extern size_t g_refuseNothrowScalarsAbove;
}  // namespace opf_test_hooks

namespace {

class MetadataSidecarReadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / ("sidecar_read_" + std::string(info->name()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    book_ = (dir_ / "Some Book.txt").generic_string();
    std::ofstream(book_) << "text";
  }
  void TearDown() override { fs::remove_all(dir_); }

  void writeSidecar(const std::string& xml) const { std::ofstream(dir_ / "Some Book.opf", std::ios::binary) << xml; }

  fs::path dir_;
  std::string book_;
};

const char* kCalibreSidecar =
    "<?xml version='1.0' encoding='utf-8'?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\">\n"
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
    "    <dc:title>A Wizard of Earthsea</dc:title>\n"
    "    <dc:creator opf:file-as=\"Le Guin, Ursula K.\" opf:role=\"aut\">Ursula K. Le Guin</dc:creator>\n"
    "    <meta name=\"calibre:series\" content=\"Earthsea\"/>\n"
    "    <meta name=\"calibre:series_index\" content=\"1\"/>\n"
    "    <dc:identifier opf:scheme=\"ISBN\">9780547773742</dc:identifier>\n"
    "    <dc:identifier opf:scheme=\"MOBI-ASIN\">B008H8SP6Q</dc:identifier>\n"
    "  </metadata>\n"
    "</package>\n";

TEST_F(MetadataSidecarReadTest, ReadsTheSidecarOfABookThatIsNotAnEpub) {
  writeSidecar(kCalibreSidecar);
  MetadataSidecarFields out;
  ASSERT_EQ(MetadataSidecar::read(book_, out), MetadataSidecar::Result::Read);
  EXPECT_EQ(out.title, "A Wizard of Earthsea");
  EXPECT_EQ(out.author, "Ursula K. Le Guin");
  EXPECT_EQ(out.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(out.authorSort, "Le Guin, Ursula K.");
  EXPECT_EQ(out.series, "Earthsea");
  EXPECT_EQ(out.seriesIndex, "1");
  EXPECT_EQ(out.isbn, "9780547773742");
  EXPECT_EQ(out.asin, "B008H8SP6Q");
}

// Contract with the File Manager's Details dialog (src/network/html/js/metadata-editor.js), which edits
// sidecars for every book format: the document its freshDoc() + writeInto() produce, with the "Sort
// author as" field filled, must give the Library its primary author and filing name. The editor runs in
// a browser and cannot be tested here, so this pins the shape the two sides agree on.
TEST_F(MetadataSidecarReadTest, ReadsTheShapeTheMetadataEditorWritesWithASortName) {
  writeSidecar(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uuid_id\">\n"
      "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">"
      "<dc:title>The Dispossessed</dc:title>"
      "<dc:creator opf:file-as=\"Le Guin, Ursula K.\">Ursula K. Le Guin</dc:creator>"
      "</metadata>\n"
      "</package>\n");
  MetadataSidecarFields out;
  ASSERT_EQ(MetadataSidecar::read(book_, out), MetadataSidecar::Result::Read);
  EXPECT_EQ(out.title, "The Dispossessed");
  EXPECT_EQ(out.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(out.authorSort, "Le Guin, Ursula K.");
}

TEST_F(MetadataSidecarReadTest, NoSidecarIsNone) {
  MetadataSidecarFields out;
  EXPECT_EQ(MetadataSidecar::read(book_, out), MetadataSidecar::Result::None);
}

// These leave the book titled by its filename.
TEST_F(MetadataSidecarReadTest, AnEmptySidecarIsIgnored) {
  writeSidecar("");
  MetadataSidecarFields out;
  EXPECT_EQ(MetadataSidecar::read(book_, out), MetadataSidecar::Result::Ignored);
}

// Whether the streaming parser reports an unclosed document as an error or simply never completes
// the field, what must hold is that nothing from it reaches the book (the same shape as
// MetadataSidecarFixture.MalformedSidecarIsIgnored in the pipeline suite).
TEST_F(MetadataSidecarReadTest, AMalformedSidecarGivesNothing) {
  writeSidecar("<package><metadata><dc:title>Unclosed");
  MetadataSidecarFields out;
  EXPECT_NE(MetadataSidecar::read(book_, out), MetadataSidecar::Result::Unavailable)
      << "a malformed sidecar is permanent until edited; retrying it on every visit gains nothing";
  EXPECT_TRUE(out.title.empty());
  EXPECT_TRUE(out.author.empty());
  EXPECT_TRUE(out.primaryAuthor.empty());
}

TEST_F(MetadataSidecarReadTest, AnOversizedSidecarIsIgnored) {
  writeSidecar(std::string(kCalibreSidecar) + "<!--" + std::string(17000, 'x') + "-->");
  MetadataSidecarFields out;
  EXPECT_EQ(MetadataSidecar::read(book_, out), MetadataSidecar::Result::Ignored);
}

// I3: a sidecar that is there but cannot be read right now -- here the parser's ~10 KB state is
// refused, as on a device short of heap -- is not "no sidecar". A caller that recorded it as such
// would label the book by its filename until the sidecar is edited.
TEST_F(MetadataSidecarReadTest, ASidecarThatCannotBeReadForWantOfMemoryIsUnavailable) {
  writeSidecar(kCalibreSidecar);
  MetadataSidecarFields out;
  opf_test_hooks::g_refuseNothrowScalarsAbove = 4096;
  const auto result = MetadataSidecar::read(book_, out);
  opf_test_hooks::g_refuseNothrowScalarsAbove = 0;
  EXPECT_EQ(result, MetadataSidecar::Result::Unavailable);
  EXPECT_TRUE(out.title.empty());
}

// The filing name belongs to the name it files.
TEST(MetadataSidecarOverlay, ASidecarWithoutAnAuthorChangesNothing) {
  std::string primary = "Terry Pratchett";
  std::string sort = "Pratchett, Terry";
  MetadataSidecar::overlayPrimaryAuthor(MetadataSidecarFields{}, primary, sort);
  EXPECT_EQ(primary, "Terry Pratchett");
  EXPECT_EQ(sort, "Pratchett, Terry");
}

TEST(MetadataSidecarOverlay, ADifferentAuthorWithoutFileAsDropsTheBooksFileAs) {
  std::string primary = "Terry Pratchett";
  std::string sort = "Pratchett, Terry";
  MetadataSidecarFields sidecar;
  sidecar.primaryAuthor = "Neil Gaiman";
  MetadataSidecar::overlayPrimaryAuthor(sidecar, primary, sort);
  EXPECT_EQ(primary, "Neil Gaiman");
  EXPECT_EQ(sort, "");
}

TEST(MetadataSidecarOverlay, TheSameAuthorWithoutFileAsKeepsTheBooksFileAs) {
  std::string primary = "Terry Pratchett";
  std::string sort = "Pratchett, Terry";
  MetadataSidecarFields sidecar;
  sidecar.primaryAuthor = "Terry Pratchett";
  MetadataSidecar::overlayPrimaryAuthor(sidecar, primary, sort);
  EXPECT_EQ(primary, "Terry Pratchett");
  EXPECT_EQ(sort, "Pratchett, Terry");
}

TEST(MetadataSidecarOverlay, ASidecarFileAsWins) {
  std::string primary = "Terry Pratchett";
  std::string sort = "Pratchett, Terry";
  MetadataSidecarFields sidecar;
  sidecar.primaryAuthor = "Terry Pratchett";
  sidecar.authorSort = "Pratchett, Sir Terry";
  MetadataSidecar::overlayPrimaryAuthor(sidecar, primary, sort);
  EXPECT_EQ(sort, "Pratchett, Sir Terry");
}

}  // namespace

#include <gtest/gtest.h>

#include <string>

#include "ContentOpfParser.h"
#include "Epub/BookMetadataCache.h"

namespace {

void parse(ContentOpfParser& parser, const std::string& xml) {
  ASSERT_TRUE(parser.setup());
  EXPECT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
}

}  // namespace

TEST(ContentOpfParserMetadata, EntityCallbackDoesNotSplitOneAuthor) {
  const std::string xml =
      R"(<package xmlns:dc="urn:dc"><metadata><dc:creator>&#201;mile Zola</dc:creator></metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.author, "Émile Zola");
}

TEST(ContentOpfParserMetadata, ClampsOversizedMetadataTextInsteadOfGrowingUnbounded) {
  const std::string hugeTitle(64 * 1024, 'A');
  const std::string xml =
      "<package xmlns:dc=\"urn:dc\"><metadata><dc:title>" + hugeTitle + " tail</dc:title></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title.size(), 512u);
  EXPECT_EQ(parser.title[0], 'A');
}

TEST(ContentOpfParserMetadata, SeparatesCreatorElementsAndCollapsesXmlWhitespace) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>  The
   Left Hand   of Darkness  </dc:title>
    <dc:creator> Ursula   K. Le Guin </dc:creator>
    <dc:creator>
Octavia E. Butler
</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "The Left Hand of Darkness");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin, Octavia E. Butler");
}

TEST(ContentOpfParserMetadata, StopsBeforeManifestWithoutOpeningTemporaryStorage) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>A Wizard of Earthsea</dc:title>
    <dc:creator>Ursula K. Le Guin</dc:creator>
    <dc:language>en</dc:language>
  </metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
  </package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.title, "A Wizard of Earthsea");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin");
  EXPECT_EQ(parser.language, "en");
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserMetadata, NeverEntersManifestWhenMetadataElementIsMissing) {
  const std::string xml =
      R"(<package><manifest><item id="chapter" href="chapter.xhtml"/></manifest><spine/></package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserCover, ResolvesEpub2CoverWithoutReadingCacheStorage) {
  const std::string xml = R"(<package><metadata><meta name="cover" content="cover-id"/></metadata>
    <manifest><item id="cover-id" href="cover.jpg" media-type="image/jpeg"/>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
    <spine><itemref idref="chapter"/></spine>
    <guide><reference type="cover" href="cover.xhtml"/></guide></package>)";
  const std::string cachePath = "/missing-cache";
  const std::string basePath = "OPS/";
  Storage = {};
  {
    ContentOpfParser parser(cachePath, basePath, xml.size(), nullptr);
    parse(parser, xml);
    EXPECT_EQ(parser.coverItemHref, "OPS/cover.jpg");
    EXPECT_EQ(parser.guideCoverPageHref, "OPS/cover.xhtml");
  }
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserCover, ResolvesEpub3CoverWithoutReadingCacheStorage) {
  const std::string xml = R"(<package><metadata/>
    <manifest><item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/></manifest>
    <spine/></package>)";
  const std::string cachePath = "/missing-cache";
  const std::string basePath = "OPS/";
  Storage = {};
  {
    ContentOpfParser parser(cachePath, basePath, xml.size(), nullptr);
    parse(parser, xml);
    EXPECT_EQ(parser.coverItemHref, "OPS/cover.png");
  }
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserCover, ReadingParserStillOpensManifestCache) {
  const std::string xml = R"(<package><metadata/>
    <manifest><item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/></manifest>
    <spine/></package>)";
  const std::string cachePath = "/reading-cache";
  const std::string basePath = "OPS/";
  BookMetadataCache cache;
  Storage = {};
  {
    ContentOpfParser parser(cachePath, basePath, xml.size(), &cache);
    parse(parser, xml);
    EXPECT_EQ(parser.coverItemHref, "OPS/cover.png");
  }
  EXPECT_EQ(Storage.writeOpens, 1);
  EXPECT_EQ(Storage.readOpens, 1);
}

// --- file-as sort forms --------------------------------------------------------

TEST(ContentOpfParserMetadata, ReadsFileAsForTitleAndEveryCreator) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title id="title">リアデイルの大地にて</dc:title>
    <meta refines="#title" property="file-as">リアデイルノダイチニテ</meta>
    <dc:creator id="creator01">Ceez</dc:creator>
    <meta refines="#creator01" property="role" scheme="marc:relators">aut</meta>
    <meta refines="#creator01" property="file-as">シーズ</meta>
    <dc:creator id="creator02">てんまそ</dc:creator>
    <meta refines="#creator02" property="role" scheme="marc:relators">ill</meta>
    <meta refines="#creator02" property="file-as">テンマソ</meta>
    <meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "リアデイルの大地にて");
  EXPECT_EQ(parser.titleFileAs, "リアデイルノダイチニテ");
  EXPECT_EQ(parser.author, "Ceez, てんまそ");
  EXPECT_EQ(parser.authorFileAs, "シーズ, テンマソ");
}

TEST(ContentOpfParserMetadata, RefinesMayPrecedeTheElementItRefines) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <meta refines="#t" property="file-as">Hobbit, The</meta>
    <meta refines="#c" property="file-as">Tolkien, J. R. R.</meta>
    <dc:title id="t">The Hobbit</dc:title>
    <dc:creator id="c">J. R. R. Tolkien</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.titleFileAs, "Hobbit, The");
  EXPECT_EQ(parser.authorFileAs, "Tolkien, J. R. R.");
}

TEST(ContentOpfParserMetadata, Epub2FileAsAttributeIsTheSortForm) {
  const std::string xml =
      R"(<package xmlns:dc="urn:dc" xmlns:opf="urn:opf"><metadata>
    <dc:title>Germinal</dc:title>
    <dc:creator opf:role="aut" opf:file-as="Zola, Emile">&#201;mile Zola</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.author, "Émile Zola");
  EXPECT_EQ(parser.authorFileAs, "Zola, Emile");
  EXPECT_TRUE(parser.titleFileAs.empty());
}

TEST(ContentOpfParserMetadata, AuthorFileAsNeedsTheFirstCreatorAndFillsInTheOthers) {
  // Only the illustrator carries a reading: no sort form at all, since the
  // shelf would otherwise file the book under the illustrator.
  const std::string illustratorOnly = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:creator id="a">山田太郎</dc:creator>
    <dc:creator id="b">鈴木花子</dc:creator>
    <meta refines="#b" property="file-as">スズキ ハナコ</meta>
  </metadata></package>)";
  ContentOpfParser first("", "", illustratorOnly.size(), nullptr);
  parse(first, illustratorOnly);
  EXPECT_EQ(first.author, "山田太郎, 鈴木花子");
  EXPECT_TRUE(first.authorFileAs.empty());

  // Only the author carries one: the illustrator's display name stands in, so
  // the list still mirrors `author` creator for creator.
  const std::string authorOnly = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:creator id="a">山田太郎</dc:creator>
    <meta refines="#a" property="file-as">ヤマダ タロウ</meta>
    <dc:creator id="b">鈴木花子</dc:creator>
  </metadata></package>)";
  ContentOpfParser second("", "", authorOnly.size(), nullptr);
  parse(second, authorOnly);
  EXPECT_EQ(second.authorFileAs, "ヤマダ タロウ, 鈴木花子");
}

TEST(ContentOpfParserMetadata, FileAsIsStillReadWhenParsingStopsAtTheManifest) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title id="t">漢字</dc:title>
    <meta refines="#t" property="file-as">かんじ</meta>
  </metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
  </package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.titleFileAs, "かんじ");
  EXPECT_EQ(Storage.writeOpens, 0);
}

TEST(ContentOpfParserMetadata, FileAsRefiningAnUnknownIdIsDroppedWithoutHarm) {
  // A refinement whose target never appears stays queued until </metadata>;
  // resolving it there must not re-queue it into the vector being walked.
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <meta refines="#nobody" property="file-as">Ghost</meta>
    <meta refines="#missing-too" property="file-as">Wraith</meta>
    <dc:title id="t">Emma</dc:title>
    <meta refines="#t" property="file-as">Emma</meta>
    <dc:creator id="c">Jane Austen</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.titleFileAs, "Emma");
  EXPECT_TRUE(parser.authorFileAs.empty());
}

TEST(ContentOpfParserMetadata, OverlongIdsAreDroppedNotTruncated) {
  // An id is matched exactly, so one past the bound is ignored whole: its
  // reading is lost, but a truncated copy could never collide with another id.
  const std::string longId(65, 'x');
  const std::string okId(64, 'y');
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title id=")" + longId +
                          R"(">Emma</dc:title>
    <meta refines="#)" + longId +
                          R"(" property="file-as">Emma reading</meta>
    <dc:creator id=")" + okId +
                          R"(">Jane Austen</dc:creator>
    <meta refines="#)" + okId +
                          R"(" property="file-as">Austen, Jane</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "Emma");
  EXPECT_TRUE(parser.titleFileAs.empty());
  EXPECT_EQ(parser.authorFileAs, "Austen, Jane");
}

TEST(ContentOpfParserMetadata, ClampsOversizedEpub2FileAsAttribute) {
  const std::string hugeReading(64 * 1024, 'R');
  const std::string xml = R"(<package xmlns:dc="urn:dc" xmlns:opf="urn:opf"><metadata>
    <dc:title opf:file-as=")" +
                          hugeReading + R"(">Germinal</dc:title>
    <dc:creator opf:file-as=")" +
                          hugeReading + R"(">Emile Zola</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.titleFileAs.size(), 512u);
  EXPECT_EQ(parser.authorFileAs.size(), 512u);
}

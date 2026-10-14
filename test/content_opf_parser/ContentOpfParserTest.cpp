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

TEST(ContentOpfParserMetadata, ExtractsIsbnAsinAndCalibreSeries) {
  const std::string xml = R"(<package xmlns:dc="urn:dc" xmlns:opf="urn:opf"><metadata>
    <dc:identifier opf:scheme="ISBN">978-1-4028-9462-6</dc:identifier>
    <dc:identifier opf:scheme="MOBI-ASIN">B0DTT5LV77</dc:identifier>
    <meta name="calibre:series" content="The Expanse"/>
    <meta name="calibre:series_index" content="3.5"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.isbn, "978-1-4028-9462-6");
  EXPECT_EQ(parser.asin, "B0DTT5LV77");
  EXPECT_EQ(parser.series, "The Expanse");
  ASSERT_TRUE(parser.seriesIndex.has_value());
  EXPECT_FLOAT_EQ(*parser.seriesIndex, 3.5f);
}

TEST(ContentOpfParserMetadata, ExtractsAmazonSchemeAsin) {
  const std::string xml = R"(<package xmlns:dc="urn:dc" xmlns:opf="urn:opf"><metadata>
    <dc:identifier opf:scheme="AMAZON">B0BF8Y54MS</dc:identifier>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.asin, "B0BF8Y54MS");
}

TEST(ContentOpfParserMetadata, ExtractsPrefixedIdentifiers) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:identifier>ISBN: 9781234567890</dc:identifier>
    <dc:identifier>ASIN: B012345678</dc:identifier>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.isbn, "9781234567890");
  EXPECT_EQ(parser.asin, "B012345678");
}

TEST(ContentOpfParserMetadata, ExtractsUrnIdentifiers) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:identifier>urn:isbn:9781234567890</dc:identifier>
    <dc:identifier>urn:asin:B012345678</dc:identifier>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.isbn, "9781234567890");
  EXPECT_EQ(parser.asin, "B012345678");
}

TEST(ContentOpfParserMetadata, ClampsOversizedMetadataAttributes) {
  const std::string hugeSeries(64 * 1024, 'S');
  const std::string xml =
      R"(<package><metadata><meta name="calibre:series" content=")" + hugeSeries + R"("/></metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series.size(), 512u);
  EXPECT_EQ(parser.series[0], 'S');
}

TEST(ContentOpfParserMetadata, ExtractsEpub3SeriesCollection) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <meta id="series-1" property="belongs-to-collection">Murderbot Diaries</meta>
    <meta refines="#series-1" property="group-position">2</meta>
    <meta refines="#series-1" property="collection-type">series</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Murderbot Diaries");
  ASSERT_TRUE(parser.seriesIndex.has_value());
  EXPECT_FLOAT_EQ(*parser.seriesIndex, 2.0f);
}

TEST(ContentOpfParserMetadata, ResolvesRefinementsBeforeCollectionDeclaration) {
  const std::string xml = R"(<package><metadata>
    <meta refines="#series-a" property="collection-type">series</meta>
    <meta refines="#series-a" property="group-position">7</meta>
    <meta id="series-a" property="belongs-to-collection">Deferred Series</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Deferred Series");
  ASSERT_TRUE(parser.seriesIndex.has_value());
  EXPECT_FLOAT_EQ(*parser.seriesIndex, 7.0f);
}

TEST(ContentOpfParserMetadata, ResolvesInterleavedCollectionRefinementsById) {
  const std::string xml = R"(<package><metadata>
    <meta id="series-a" property="belongs-to-collection">Primary Series</meta>
    <meta id="series-b" property="belongs-to-collection">Secondary Series</meta>
    <meta refines="#series-a" property="collection-type">series</meta>
    <meta refines="#series-a" property="group-position">3</meta>
    <meta refines="#series-b" property="collection-type">series</meta>
    <meta refines="#series-b" property="group-position">9</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Primary Series");
  ASSERT_TRUE(parser.seriesIndex.has_value());
  EXPECT_FLOAT_EQ(*parser.seriesIndex, 3.0f);
}

TEST(ContentOpfParserMetadata, KeepsSeriesIndexWithSelectedMetadataSource) {
  const std::string xml = R"(<package><metadata>
    <meta name="calibre:series" content="Calibre Series"/>
    <meta name="calibre:series_index" content="4"/>
    <meta id="epub-series" property="belongs-to-collection">EPUB Series</meta>
    <meta refines="#epub-series" property="collection-type">series</meta>
    <meta refines="#epub-series" property="group-position">9</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Calibre Series");
  ASSERT_TRUE(parser.seriesIndex.has_value());
  EXPECT_FLOAT_EQ(*parser.seriesIndex, 4.0f);
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

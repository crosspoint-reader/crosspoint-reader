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
  EXPECT_EQ(parser.seriesIndexText, "3.5");
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
  EXPECT_EQ(parser.seriesIndexText, "2");
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
  EXPECT_EQ(parser.seriesIndexText, "7");
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
  EXPECT_EQ(parser.seriesIndexText, "3");
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
  EXPECT_EQ(parser.seriesIndexText, "4");
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

TEST(ContentOpfParserSeriesCalibre, ReadsNameAndIndex) {
  const std::string xml = R"(<package><metadata>
    <meta name="calibre:series" content="Discworld"/>
    <meta name="calibre:series_index" content="5"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Discworld");
  EXPECT_EQ(parser.seriesIndexText, "5");
}

TEST(ContentOpfParserSeriesCalibre, ReadsAFractionalIndex) {
  const std::string xml = R"(<package><metadata>
    <meta name="calibre:series" content="Discworld"/>
    <meta name="calibre:series_index" content="16.5"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.seriesIndexText, "16.5");
}

TEST(ContentOpfParserSeriesCalibre, SurvivesAMissingIndex) {
  const std::string xml = R"(<package><metadata>
    <meta name="calibre:series" content="Discworld"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Discworld");
  EXPECT_TRUE(parser.seriesIndexText.empty());
}

TEST(ContentOpfParserSeriesCalibre, DecodesEntitiesInTheName) {
  const std::string xml = R"(<package><metadata>
    <meta name="calibre:series" content="Fire &amp; Blood"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Fire & Blood");
}

TEST(ContentOpfParserSeriesCalibre, IgnoresACommentedOutMeta) {
  const std::string xml = R"(<package><metadata>
    <!-- <meta name="calibre:series" content="Ghost Series"/> -->
    <meta name="calibre:series" content="Discworld"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Discworld");
}

TEST(ContentOpfParserSeriesCalibre, ToleratesARawGreaterThanInAnAttributeValue) {
  const std::string xml = R"(<package><metadata>
    <meta name="calibre:series" content="A > B"/>
    <meta name="calibre:series_index" content="2"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "A > B");
  EXPECT_EQ(parser.seriesIndexText, "2");
}

TEST(ContentOpfParserSeriesEpub3, ReadsCollectionAndGroupPosition) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c1">The Wheel of Time</meta>
    <meta refines="#c1" property="collection-type">series</meta>
    <meta refines="#c1" property="group-position">3</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "The Wheel of Time");
  EXPECT_EQ(parser.seriesIndexText, "3");
}

TEST(ContentOpfParserSeriesEpub3, AcceptsACollectionWithNoDeclaredType) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c1">Earthsea</meta>
    <meta refines="#c1" property="group-position">2</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Earthsea");
  EXPECT_EQ(parser.seriesIndexText, "2");
}

TEST(ContentOpfParserSeriesEpub3, AcceptsAMiscasedCollectionType) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c1">Earthsea</meta>
    <meta refines="#c1" property="collection-type">Series</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Earthsea");
}

TEST(ContentOpfParserSeriesEpub3, IgnoresABoxedSet) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c1">Complete Works</meta>
    <meta refines="#c1" property="collection-type">set</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_TRUE(parser.series.empty());
}

TEST(ContentOpfParserSeriesEpub3, PrefersTheSeriesOverABoxedSetDeclaredBeforeIt) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="box">Complete Works</meta>
    <meta refines="#box" property="collection-type">set</meta>
    <meta property="belongs-to-collection" id="ser">Earthsea</meta>
    <meta refines="#ser" property="collection-type">series</meta>
    <meta refines="#ser" property="group-position">4</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Earthsea");
  EXPECT_EQ(parser.seriesIndexText, "4");
}

TEST(ContentOpfParserSeriesEpub3, PrefersAnExplicitSeriesOverAnUntypedCollection) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="a">Some Anthology</meta>
    <meta property="belongs-to-collection" id="b">Earthsea</meta>
    <meta refines="#b" property="collection-type">series</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Earthsea");
}

TEST(ContentOpfParserSeriesEpub3, TakesTheFirstUntypedCollectionWhenNoneClaimsToBeASeries) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="a">First</meta>
    <meta property="belongs-to-collection" id="b">Second</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "First");
}

TEST(ContentOpfParserSeriesEpub3, FallsBackPastACollectionWhoseNameIsBlank) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="a">   </meta>
    <meta property="belongs-to-collection" id="b">Earthsea</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Earthsea");
}

TEST(ContentOpfParserSeriesEpub3, DoesNotTakeAPositionThatRefinesSomethingElse) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c1">Earthsea</meta>
    <meta refines="#other" property="group-position">9</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Earthsea");
  EXPECT_TRUE(parser.seriesIndexText.empty());
}

TEST(ContentOpfParserSeriesEpub3, TrimsTheCollectionName) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c1">
      The   Wheel of Time
    </meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "The Wheel of Time");
}

TEST(ContentOpfParserSeriesEpub3, ResolvesARefineThatPrecedesItsCollection) {
  const std::string xml = R"(<package><metadata>
    <meta refines="#c1" property="collection-type">series</meta>
    <meta refines="#c1" property="group-position">7</meta>
    <meta property="belongs-to-collection" id="c1">Earthsea</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Earthsea");
  EXPECT_EQ(parser.seriesIndexText, "7");
}

TEST(ContentOpfParserSeriesPrecedence, CalibreWinsWhenABookCarriesBoth) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c1">Publisher Collection</meta>
    <meta refines="#c1" property="group-position">9</meta>
    <meta name="calibre:series" content="Discworld"/>
    <meta name="calibre:series_index" content="5"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Discworld");
  EXPECT_EQ(parser.seriesIndexText, "5");
}

TEST(ContentOpfParserSeriesPrecedence, FallsBackToEpub3WhenTheCalibreNameIsBlank) {
  const std::string xml = R"(<package><metadata>
    <meta name="calibre:series" content="  "/>
    <meta property="belongs-to-collection" id="c1">Earthsea</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Earthsea");
}

TEST(ContentOpfParserSeriesAbsent, LeavesTheFieldsEmpty) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>A Standalone</dc:title>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_TRUE(parser.series.empty());
  EXPECT_TRUE(parser.seriesIndexText.empty());
}

TEST(ContentOpfParserSeriesAbsent, DoesNotDisturbTitleOrAuthor) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>Small Gods</dc:title>
    <dc:creator>Terry Pratchett</dc:creator>
    <meta name="calibre:series" content="Discworld"/>
    <meta name="calibre:series_index" content="13"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "Small Gods");
  EXPECT_EQ(parser.author, "Terry Pratchett");
  EXPECT_EQ(parser.series, "Discworld");
}

TEST(ContentOpfParserPublisher, ReadsThePublisher) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:publisher> Victor   Gollancz </dc:publisher>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.publisher, "Victor Gollancz");
}

TEST(ContentOpfParserPublisher, FirstOneWinsWhenRepeated) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:publisher>Gollancz</dc:publisher>
    <dc:publisher>Orbit</dc:publisher>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.publisher, "Gollancz");
}

TEST(ContentOpfParserPublisher, ClampsOversizedText) {
  const std::string hugePublisher(64 * 1024, 'P');
  const std::string xml = "<package xmlns:dc=\"urn:dc\"><metadata><dc:publisher>" + hugePublisher +
                          " tail</dc:publisher></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.publisher.size(), 512u);
  EXPECT_EQ(parser.publisher[0], 'P');
}

TEST(ContentOpfParserSubject, ReadsTheSubject) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:subject>Science &amp; Fiction</dc:subject>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.subject, "Science & Fiction");
}

TEST(ContentOpfParserSubject, FirstOneWinsWhenRepeated) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:subject>Fantasy</dc:subject>
    <dc:subject>Humour</dc:subject>
    <dc:subject>Satire</dc:subject>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.subject, "Fantasy");
}

TEST(ContentOpfParserSubject, SkipsABlankFirstElement) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:subject>  </dc:subject>
    <dc:subject>Fantasy</dc:subject>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.subject, "Fantasy");
}

TEST(ContentOpfParserSubject, ClampsOversizedText) {
  const std::string hugeSubject(64 * 1024, 'S');
  const std::string xml =
      "<package xmlns:dc=\"urn:dc\"><metadata><dc:subject>" + hugeSubject + " tail</dc:subject></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.subject.size(), 512u);
  EXPECT_EQ(parser.subject[0], 'S');
}

TEST(ContentOpfParserLanguage, FirstOneWinsWhenRepeated) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:language>en-GB</dc:language>
    <dc:language>fr</dc:language>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.language, "en-GB");
}

TEST(ContentOpfParserPublisher, LeavesPublisherAndSubjectEmptyWhenAbsent) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>A Book</dc:title>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_TRUE(parser.publisher.empty());
  EXPECT_TRUE(parser.subject.empty());
}

TEST(ContentOpfParserSeriesCalibre, BoundsLargeAttributesAndTruncatesOnUtf8Boundaries) {
  for (const std::string& value : {std::string(64 * 1024, 'A'), std::string(511, 'A') + "é tail"}) {
    const std::string xml = "<package><metadata><meta name=\"calibre:series\" content=\"" + value +
                            "\"/><meta name=\"calibre:series_index\" content=\"" + std::string(65536, '9') +
                            "\"/></metadata></package>";
    ContentOpfParser parser("", "", xml.size(), nullptr);
    parse(parser, xml);
    EXPECT_EQ(parser.series, std::string(value[511] == 'A' ? 512 : 511, 'A'));
    EXPECT_EQ(parser.seriesIndexText.size(), 512u);
  }
}

TEST(ContentOpfParserSeriesEpub3, LongRefinesMatchFullIdsInsteadOfTheirPrefix) {
  const std::string prefix(64 * 1024, 'x');
  const std::string xml = "<package><metadata><meta property=\"belongs-to-collection\" id=\"" + prefix +
                          "a\">Earthsea</meta><meta refines=\"#" + prefix +
                          "b\" property=\"group-position\">9</meta>"
                          "<meta refines=\"#" +
                          prefix + "a\" property=\"group-position\">2</meta></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_EQ(parser.series, "Earthsea");
  EXPECT_EQ(parser.seriesIndexText, "2");
}

TEST(ContentOpfParserMetadata, Utf8AndCreatorSeparatorsRespectTheTextLimitAcrossCallbacks) {
  const std::string prefix(511, 'A');
  const std::string xml =
      "<package xmlns:dc=\"urn:dc\"><metadata><dc:title>" + prefix + "é tail</dc:title><dc:creator>" + prefix +
      "</dc:creator><dc:creator>Other</dc:creator>"
      "<dc:publisher>" +
      prefix + "é tail</dc:publisher><dc:subject>" + prefix + "é tail</dc:subject></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);
  ASSERT_TRUE(parser.setup());
  // Streaming one byte at a time splits UTF-8 and XML callback boundaries.
  for (const unsigned char c : xml) ASSERT_EQ(parser.write(c), 1u);
  EXPECT_EQ(parser.title, prefix);
  EXPECT_EQ(parser.author, prefix);
  EXPECT_EQ(parser.publisher, prefix);
  EXPECT_EQ(parser.subject, prefix);
}

#include <gtest/gtest.h>

#include <algorithm>
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

TEST(ContentOpfParserExtended, ReadsEpub2FileAsAndCalibreMeta) {
  const std::string xml = R"(<package xmlns:dc="urn:dc" xmlns:opf="urn:opf"><metadata>
    <dc:title>The Colour of Magic</dc:title>
    <dc:creator opf:role="ill" opf:file-as="Kirby, Josh">Josh Kirby</dc:creator>
    <dc:creator opf:role="aut" opf:file-as="Pratchett, Terry">Terry Pratchett</dc:creator>
    <dc:subject>Fantasy</dc:subject>
    <dc:subject>Humor</dc:subject>
    <dc:subject>fantasy</dc:subject>
    <meta name="calibre:series" content="Discworld"/>
    <meta name="calibre:series_index" content="1.0"/>
    <meta name="calibre:title_sort" content="Colour of Magic, The"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.titleSort, "Colour of Magic, The");
  EXPECT_EQ(parser.authorSort, "Pratchett, Terry");
  EXPECT_EQ(parser.series, "Discworld");
  EXPECT_EQ(parser.seriesIndex, "1");
  EXPECT_EQ(parser.tags, "Fantasy\nHumor");
}

TEST(ContentOpfParserExtended, ResolvesEpub3RefinesInEitherOrder) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <meta refines="#t" property="file-as">Hobbit, The</meta>
    <dc:title id="t">The Hobbit</dc:title>
    <dc:creator id="c1">Alan Lee</dc:creator>
    <meta refines="#c1" property="role">ill</meta>
    <meta refines="#c1" property="file-as">Lee, Alan</meta>
    <dc:creator id="c2">J. R. R. Tolkien</dc:creator>
    <meta refines="#c2" property="file-as">Tolkien, J. R. R.</meta>
    <meta property="belongs-to-collection" id="set">Tolkien Box</meta>
    <meta refines="#set" property="collection-type">set</meta>
    <meta property="belongs-to-collection" id="s">Middle-earth</meta>
    <meta refines="#s" property="collection-type">series</meta>
    <meta refines="#s" property="group-position">2.50</meta>
    <dc:subject>Fantasy</dc:subject>
    <meta property="schema:genre">FANTASY</meta>
    <meta property="schema:genre">Adventure</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.titleSort, "Hobbit, The");
  EXPECT_EQ(parser.authorSort, "Tolkien, J. R. R.");
  EXPECT_EQ(parser.series, "Middle-earth");
  EXPECT_EQ(parser.seriesIndex, "2.5");
  EXPECT_EQ(parser.tags, "Fantasy\nAdventure");
}

TEST(ContentOpfParserExtended, CalibreSeriesBeatsCollectionAndSetNeverNamesSeries) {
  const std::string setOnly = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c">Penguin Classics</meta>
    <meta refines="#c" property="collection-type">set</meta>
  </metadata></package>)";
  ContentOpfParser setParser("", "", setOnly.size(), nullptr);
  parse(setParser, setOnly);
  EXPECT_EQ(setParser.series, "");

  const std::string both = R"(<package><metadata>
    <meta property="belongs-to-collection" id="c">Collection Name</meta>
    <meta name="calibre:series" content="Calibre Name"/>
    <meta name="calibre:series_index" content="3"/>
  </metadata></package>)";
  ContentOpfParser bothParser("", "", both.size(), nullptr);
  parse(bothParser, both);
  EXPECT_EQ(bothParser.series, "Calibre Name");
  EXPECT_EQ(bothParser.seriesIndex, "3");
}

TEST(ContentOpfParserExtended, MissingSortKeysStayEmptyAndPlainMetadataIsUnchanged) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>Dracula</dc:title>
    <dc:creator>Bram Stoker</dc:creator>
    <meta property="dcterms:modified">2020-01-01</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "Dracula");
  EXPECT_EQ(parser.author, "Bram Stoker");
  EXPECT_EQ(parser.titleSort, "");
  EXPECT_EQ(parser.authorSort, "");
  EXPECT_EQ(parser.series, "");
  EXPECT_EQ(parser.seriesIndex, "");
  EXPECT_EQ(parser.tags, "");
}

TEST(ContentOpfParserExtended, CapsTagCount) {
  std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>)";
  for (int i = 0; i < 40; i++) xml += "<dc:subject>tag" + std::to_string(i) + "</dc:subject>";
  xml += "</metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  const auto separators = std::count(parser.tags.begin(), parser.tags.end(), ContentOpfParser::TAG_SEPARATOR);
  EXPECT_EQ(separators, 15);
  EXPECT_EQ(parser.tags.substr(0, 10), "tag0\ntag1\n");
}

TEST(ContentOpfParserExtended, MetadataOnlyParseStillFinalizes) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:creator id="a">Mary Shelley</dc:creator>
    <meta refines="#a" property="file-as">Shelley, Mary</meta>
    <dc:subject>Horror</dc:subject>
  </metadata><manifest><item id="x" href="x.xhtml"/></manifest></package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.authorSort, "Shelley, Mary");
  EXPECT_EQ(parser.tags, "Horror");
}

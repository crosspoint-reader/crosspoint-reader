#include <gtest/gtest.h>

#include <string>

#include "ContentOpfParser.h"

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

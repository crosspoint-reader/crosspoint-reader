#include <OpdsParser.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ParsedFeed {
  std::vector<OpdsEntry> entries;
  bool error = false;
  bool truncated = false;
};

ParsedFeed parseFeed(const std::string& xml) {
  OpdsParser parser;
  parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
  parser.flush();
  const bool error = parser.error();
  const bool truncated = parser.truncated();
  return {std::move(parser).getEntries(), error, truncated};
}

TEST(OpdsParser, MapsAcquisitionFormatsToStableLabelsAndExtensions) {
  EXPECT_STREQ(opdsAcquisitionLabel(OpdsAcquisitionFormat::EPUB), "EPUB");
  EXPECT_STREQ(opdsAcquisitionExtension(OpdsAcquisitionFormat::EPUB), ".epub");
  EXPECT_STREQ(opdsAcquisitionLabel(OpdsAcquisitionFormat::XTC), "XTC");
  EXPECT_STREQ(opdsAcquisitionExtension(OpdsAcquisitionFormat::XTC), ".xtc");
  EXPECT_STREQ(opdsAcquisitionLabel(OpdsAcquisitionFormat::XTCH), "XTCH");
  EXPECT_STREQ(opdsAcquisitionExtension(OpdsAcquisitionFormat::XTCH), ".xtch");
}

TEST(OpdsParser, KeepsExistingEpubAcquisitions) {
  const auto feed = parseFeed(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <entry>
        <title>EPUB book</title>
        <link rel="http://opds-spec.org/acquisition"
              type="application/epub+zip" href="/books/epub"/>
      </entry>
    </feed>
  )xml");

  ASSERT_FALSE(feed.error);
  ASSERT_EQ(feed.entries.size(), 1u);
  const auto& entry = feed.entries.front();
  EXPECT_EQ(entry.type, OpdsEntryType::BOOK);
  EXPECT_TRUE(entry.href.empty());
  ASSERT_EQ(entry.acquisitionLinks.size(), 1u);
  EXPECT_EQ(entry.acquisitionLinks.front().format, OpdsAcquisitionFormat::EPUB);
  EXPECT_EQ(entry.acquisitionLinks.front().href, "/books/epub");
}

TEST(OpdsParser, RecognizesXtcAndXtchAcquisitionMimeTypes) {
  const auto feed = parseFeed(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <entry>
        <title>XTC book</title>
        <link rel="http://opds-spec.org/acquisition"
              type="application/x-xtc+zip" href="/books/xtc"/>
      </entry>
      <entry>
        <title>XTCH book</title>
        <link rel="http://opds-spec.org/acquisition"
              type="application/x-xtch+zip" href="/books/xtch"/>
      </entry>
    </feed>
  )xml");

  ASSERT_FALSE(feed.error);
  ASSERT_EQ(feed.entries.size(), 2u);
  ASSERT_EQ(feed.entries[0].acquisitionLinks.size(), 1u);
  ASSERT_EQ(feed.entries[1].acquisitionLinks.size(), 1u);
  EXPECT_EQ(feed.entries[0].acquisitionLinks[0].format, OpdsAcquisitionFormat::XTC);
  EXPECT_EQ(feed.entries[1].acquisitionLinks[0].format, OpdsAcquisitionFormat::XTCH);
}

TEST(OpdsParser, NormalizesMimeTypesAndRecognizesVendorAliases) {
  const auto feed = parseFeed(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <entry>
        <title>Aliases</title>
        <link rel="http://opds-spec.org/acquisition"
              type=" Application/Vnd.Xteink.Xtc ; charset=binary" href="/books/a"/>
        <link rel="http://opds-spec.org/acquisition"
              type="APPLICATION/VND.XTEINK.XTCH" href="/books/b"/>
      </entry>
    </feed>
  )xml");

  ASSERT_FALSE(feed.error);
  ASSERT_EQ(feed.entries.size(), 1u);
  const auto& links = feed.entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), 2u);
  EXPECT_EQ(links[0].format, OpdsAcquisitionFormat::XTC);
  EXPECT_EQ(links[1].format, OpdsAcquisitionFormat::XTCH);
}

TEST(OpdsParser, InfersXtcFormatsFromGenericOrMissingMimeTypes) {
  const auto feed = parseFeed(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <entry>
        <title>Suffixes</title>
        <link rel="http://opds-spec.org/acquisition"
              href="/books/VOLUME.XTC?token=1#download"/>
        <link rel="http://opds-spec.org/acquisition"
              type="application/octet-stream" href="/books/volume.XTCH/?download=1"/>
      </entry>
    </feed>
  )xml");

  ASSERT_FALSE(feed.error);
  ASSERT_EQ(feed.entries.size(), 1u);
  const auto& links = feed.entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), 2u);
  EXPECT_EQ(links[0].format, OpdsAcquisitionFormat::XTC);
  EXPECT_EQ(links[1].format, OpdsAcquisitionFormat::XTCH);
}

TEST(OpdsParser, IgnoresUnsupportedAndAmbiguousAcquisitionLinks) {
  const auto feed = parseFeed(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <entry>
        <title>Unsupported</title>
        <link rel="http://opds-spec.org/acquisition"
              type="text/plain" href="/books/not-really.xtc"/>
        <link rel="http://opds-spec.org/acquisition"
              type="application/octet-stream" href="/books/no-extension"/>
        <link rel="http://opds-spec.org/acquisition"
              type="application/x-mobipocket-ebook" href="/books/book.mobi"/>
      </entry>
    </feed>
  )xml");

  EXPECT_FALSE(feed.error);
  EXPECT_TRUE(feed.entries.empty());
}

TEST(OpdsParser, IgnoresEmptyAcquisitionUrls) {
  const auto feed = parseFeed(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <entry>
        <title>Empty URL</title>
        <link rel="http://opds-spec.org/acquisition"
              type="application/x-xtc+zip" href=""/>
      </entry>
    </feed>
  )xml");

  EXPECT_FALSE(feed.error);
  EXPECT_TRUE(feed.entries.empty());
}

TEST(OpdsParser, KeepsOneLinkPerFormatInFeedOrder) {
  const auto feed = parseFeed(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <entry>
        <title>Three formats</title>
        <link rel="http://opds-spec.org/acquisition"
              type="application/x-xtch+zip" href="/first.xtch"/>
        <link rel="http://opds-spec.org/acquisition"
              type="application/epub+zip" href="/book.epub"/>
        <link rel="http://opds-spec.org/acquisition"
              type="application/x-xtch+zip" href="/duplicate.xtch"/>
        <link rel="http://opds-spec.org/acquisition"
              type="application/x-xtc+zip" href="/book.xtc"/>
      </entry>
    </feed>
  )xml");

  ASSERT_FALSE(feed.error);
  ASSERT_EQ(feed.entries.size(), 1u);
  const auto& links = feed.entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), 3u);
  EXPECT_EQ(links[0].format, OpdsAcquisitionFormat::XTCH);
  EXPECT_EQ(links[0].href, "/first.xtch");
  EXPECT_EQ(links[1].format, OpdsAcquisitionFormat::EPUB);
  EXPECT_EQ(links[2].format, OpdsAcquisitionFormat::XTC);
}

TEST(OpdsParser, KeepsNavigationEntriesSeparateFromAcquisitions) {
  const auto feed = parseFeed(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <entry>
        <title>Browse authors</title>
        <link rel="subsection" type="application/atom+xml;profile=opds-catalog"
              href="/authors"/>
      </entry>
    </feed>
  )xml");

  ASSERT_FALSE(feed.error);
  ASSERT_EQ(feed.entries.size(), 1u);
  EXPECT_EQ(feed.entries.front().type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(feed.entries.front().href, "/authors");
  EXPECT_TRUE(feed.entries.front().acquisitionLinks.empty());
}

TEST(OpdsParser, BoundsAcquisitionUrls) {
  const std::string longUrl = "/" + std::string(900, 'a');
  const auto feed = parseFeed(
      "<feed><entry><title>Long URL</title><link "
      "rel=\"http://opds-spec.org/acquisition\" type=\"application/x-xtc+zip\" href=\"" +
      longUrl + "\"/></entry></feed>");

  ASSERT_FALSE(feed.error);
  ASSERT_EQ(feed.entries.size(), 1u);
  ASSERT_EQ(feed.entries.front().acquisitionLinks.size(), 1u);
  EXPECT_EQ(feed.entries.front().acquisitionLinks.front().href.size(), 768u);
}

TEST(OpdsParser, CapsOversizedFeeds) {
  std::string xml = "<feed>";
  for (int i = 0; i < 70; ++i) {
    xml += "<entry><title>Book " + std::to_string(i) +
           "</title><link rel=\"http://opds-spec.org/acquisition\" "
           "type=\"application/epub+zip\" href=\"/book/" +
           std::to_string(i) + "\"/></entry>";
  }
  xml += "</feed>";

  const auto feed = parseFeed(xml);

  EXPECT_FALSE(feed.error);
  EXPECT_TRUE(feed.truncated);
  EXPECT_EQ(feed.entries.size(), 62u);
}

TEST(OpdsParser, BoundsAggregateAcquisitionUrlStorage) {
  const std::string longPath = "/" + std::string(767, 'a');
  std::string xml = "<feed>";
  for (int i = 0; i < 62; ++i) {
    xml += "<entry><title>Book " + std::to_string(i) + "</title>";
    xml += "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" href=\"" + longPath + "\"/>";
    xml += "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/x-xtc+zip\" href=\"" + longPath + "\"/>";
    xml += "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/x-xtch+zip\" href=\"" + longPath +
           "\"/></entry>";
  }
  xml += "</feed>";

  const auto feed = parseFeed(xml);
  size_t storedUrlChars = 0;
  for (const auto& entry : feed.entries) {
    for (const auto& link : entry.acquisitionLinks) storedUrlChars += link.href.size();
  }

  EXPECT_FALSE(feed.error);
  EXPECT_TRUE(feed.truncated);
  EXPECT_LE(storedUrlChars, 62u * 768u);
}

}  // namespace

#include <gtest/gtest.h>

#include <cstring>
#include <algorithm>
#include <string>
#include <memory>

#include "RssParser.h"

namespace {
std::unique_ptr<RssParser> parse(const char* xml) {
  auto parser = std::make_unique<RssParser>();
  parser->write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser->flush();
  return parser;
}
}  // namespace

TEST(RssParser, ParsesBasicRssFeed) {
  auto parser = parse(R"xml(
    <rss version="2.0">
      <channel>
        <title>Example Feed</title>
        <item>
          <title>First</title>
          <link>https://example.com/first</link>
          <description>One summary</description>
          <author>Alice</author>
          <pubDate>Thu, 04 Jun 2026 12:00:00 GMT</pubDate>
          <guid>first-guid</guid>
        </item>
        <item>
          <title>Second</title>
          <link>https://example.com/second</link>
          <description>Two summary</description>
        </item>
      </channel>
    </rss>
  )xml");

  ASSERT_TRUE(*parser);
  EXPECT_EQ(parser->getFeedTitle(), "Example Feed");
  ASSERT_EQ(parser->getItems().size(), 2u);
  EXPECT_EQ(parser->getItems()[0].title, "First");
  EXPECT_EQ(parser->getItems()[0].link, "https://example.com/first");
  EXPECT_EQ(parser->getItems()[0].content, "One summary");
  EXPECT_EQ(parser->getItems()[0].author, "Alice");
  EXPECT_EQ(parser->getItems()[0].published, "Thu, 04 Jun 2026 12:00:00 GMT");
  EXPECT_EQ(parser->getItems()[0].guid, "first-guid");
}

TEST(RssParser, PrefersContentEncodedAndDecodesEntities) {
  auto parser = parse(R"xml(
    <rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/">
      <channel>
        <item>
          <title>Article</title>
          <description>Short &amp; plain</description>
          <content:encoded><![CDATA[<p>Long <strong>body</strong> &amp; details&nbsp;here.</p>]]></content:encoded>
        </item>
      </channel>
    </rss>
  )xml");

  ASSERT_TRUE(*parser);
  ASSERT_EQ(parser->getItems().size(), 1u);
  EXPECT_EQ(parser->getItems()[0].content, "Long body & details here.");
}

TEST(RssParser, ParsesNamespacedCreator) {
  auto parser = parse(R"xml(
    <rss version="2.0" xmlns:dc="http://purl.org/dc/elements/1.1/">
      <channel>
        <item>
          <title>Namespaced</title>
          <dc:creator>Casey</dc:creator>
          <description>Summary</description>
        </item>
      </channel>
    </rss>
  )xml");

  ASSERT_TRUE(*parser);
  ASSERT_EQ(parser->getItems().size(), 1u);
  EXPECT_EQ(parser->getItems()[0].author, "Casey");
}

TEST(RssParser, ParsesAtomEntryLinkHref) {
  auto parser = parse(R"xml(
    <feed xmlns="http://www.w3.org/2005/Atom">
      <title>Atom Feed</title>
      <entry>
        <title>Atom Item</title>
        <link href="https://example.com/atom-item"/>
        <summary>Atom summary</summary>
        <updated>2026-06-04T12:00:00Z</updated>
      </entry>
    </feed>
  )xml");

  ASSERT_TRUE(*parser);
  EXPECT_EQ(parser->getFeedTitle(), "Atom Feed");
  ASSERT_EQ(parser->getItems().size(), 1u);
  EXPECT_EQ(parser->getItems()[0].title, "Atom Item");
  EXPECT_EQ(parser->getItems()[0].link, "https://example.com/atom-item");
  EXPECT_EQ(parser->getItems()[0].content, "Atom summary");
  EXPECT_EQ(parser->getItems()[0].published, "2026-06-04T12:00:00Z");
}

TEST(RssParser, SupportsChunkedInput) {
  const char* xml = R"xml(
    <rss><channel><title>Chunked</title><item><title>A</title><description>B</description></item></channel></rss>
  )xml";

  RssParser parser;
  const size_t len = strlen(xml);
  for (size_t offset = 0; offset < len; offset += 3) {
    const size_t chunk = std::min<size_t>(3, len - offset);
    parser.write(reinterpret_cast<const uint8_t*>(xml + offset), chunk);
  }
  parser.flush();

  ASSERT_TRUE(parser);
  EXPECT_EQ(parser.getFeedTitle(), "Chunked");
  ASSERT_EQ(parser.getItems().size(), 1u);
  EXPECT_EQ(parser.getItems()[0].title, "A");
  EXPECT_EQ(parser.getItems()[0].content, "B");
}

TEST(RssParser, ReportsMalformedXml) {
  auto parser = parse("<rss><channel><item><title>Broken</title></channel></rss>");
  EXPECT_FALSE(*parser);
}

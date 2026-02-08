#include <cstring>

#include "lib/OpdsParser/OpdsParser.h"
#include "test/test_harness.h"

static void feedXml(OpdsParser& parser, const char* xml) {
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();
}

// --- Navigation feed ---

void testNavigationFeed() {
  const char* xml =
      "<?xml version='1.0' encoding='UTF-8'?>"
      "<feed xmlns='http://www.w3.org/2005/Atom'>"
      "  <entry>"
      "    <title>Popular</title>"
      "    <id>urn:popular</id>"
      "    <link type='application/atom+xml' href='/popular'/>"
      "  </entry>"
      "</feed>";

  OpdsParser parser;
  feedXml(parser, xml);

  ASSERT_FALSE(parser.error());
  const auto& entries = parser.getEntries();
  ASSERT_EQ(entries.size(), 1u);
  ASSERT_EQ(static_cast<int>(entries[0].type), static_cast<int>(OpdsEntryType::NAVIGATION));
  ASSERT_STREQ(entries[0].title, "Popular");
  ASSERT_STREQ(entries[0].href, "/popular");
  ASSERT_STREQ(entries[0].id, "urn:popular");
}

// --- Acquisition feed ---

void testAcquisitionFeed() {
  const char* xml =
      "<?xml version='1.0' encoding='UTF-8'?>"
      "<feed xmlns='http://www.w3.org/2005/Atom'>"
      "  <entry>"
      "    <title>Pride and Prejudice</title>"
      "    <author><name>Jane Austen</name></author>"
      "    <id>urn:isbn:12345</id>"
      "    <link rel='http://opds-spec.org/acquisition' type='application/epub+zip' href='/download/12345.epub'/>"
      "  </entry>"
      "</feed>";

  OpdsParser parser;
  feedXml(parser, xml);

  ASSERT_FALSE(parser.error());
  const auto& entries = parser.getEntries();
  ASSERT_EQ(entries.size(), 1u);
  ASSERT_EQ(static_cast<int>(entries[0].type), static_cast<int>(OpdsEntryType::BOOK));
  ASSERT_STREQ(entries[0].title, "Pride and Prejudice");
  ASSERT_STREQ(entries[0].author, "Jane Austen");
  ASSERT_STREQ(entries[0].href, "/download/12345.epub");
}

// --- Mixed entries ---

void testMixedEntries() {
  const char* xml =
      "<?xml version='1.0' encoding='UTF-8'?>"
      "<feed xmlns='http://www.w3.org/2005/Atom'>"
      "  <entry>"
      "    <title>Browse</title>"
      "    <id>1</id>"
      "    <link type='application/atom+xml' href='/browse'/>"
      "  </entry>"
      "  <entry>"
      "    <title>A Book</title>"
      "    <author><name>Author</name></author>"
      "    <id>2</id>"
      "    <link rel='http://opds-spec.org/acquisition' type='application/epub+zip' href='/book.epub'/>"
      "  </entry>"
      "</feed>";

  OpdsParser parser;
  feedXml(parser, xml);

  ASSERT_FALSE(parser.error());
  ASSERT_EQ(parser.getEntries().size(), 2u);
  ASSERT_EQ(static_cast<int>(parser.getEntries()[0].type), static_cast<int>(OpdsEntryType::NAVIGATION));
  ASSERT_EQ(static_cast<int>(parser.getEntries()[1].type), static_cast<int>(OpdsEntryType::BOOK));

  auto books = parser.getBooks();
  ASSERT_EQ(books.size(), 1u);
  ASSERT_STREQ(books[0].title, "A Book");
}

// --- Namespace prefixes ---

void testNamespacePrefixes() {
  const char* xml =
      "<?xml version='1.0' encoding='UTF-8'?>"
      "<atom:feed xmlns:atom='http://www.w3.org/2005/Atom'>"
      "  <atom:entry>"
      "    <atom:title>Catalog</atom:title>"
      "    <atom:id>urn:cat</atom:id>"
      "    <atom:link type='application/atom+xml' href='/cat'/>"
      "  </atom:entry>"
      "</atom:feed>";

  OpdsParser parser;
  feedXml(parser, xml);

  ASSERT_FALSE(parser.error());
  ASSERT_EQ(parser.getEntries().size(), 1u);
  ASSERT_STREQ(parser.getEntries()[0].title, "Catalog");
}

// --- Entry without href is skipped ---

void testEntryWithoutHref() {
  const char* xml =
      "<?xml version='1.0' encoding='UTF-8'?>"
      "<feed xmlns='http://www.w3.org/2005/Atom'>"
      "  <entry>"
      "    <title>No link</title>"
      "    <id>urn:nolink</id>"
      "  </entry>"
      "</feed>";

  OpdsParser parser;
  feedXml(parser, xml);

  ASSERT_FALSE(parser.error());
  ASSERT_EQ(parser.getEntries().size(), 0u);
}

// --- Empty feed ---

void testEmptyFeed() {
  const char* xml =
      "<?xml version='1.0' encoding='UTF-8'?>"
      "<feed xmlns='http://www.w3.org/2005/Atom'>"
      "</feed>";

  OpdsParser parser;
  feedXml(parser, xml);

  ASSERT_FALSE(parser.error());
  ASSERT_EQ(parser.getEntries().size(), 0u);
}

// --- Malformed XML ---

void testMalformedXml() {
  const char* xml = "<feed><entry><title>Broken";  // no closing tags

  OpdsParser parser;
  feedXml(parser, xml);

  // flush() on incomplete XML should produce an error
  ASSERT_TRUE(parser.error());
}

// --- Chunked writes ---

void testChunkedWrite() {
  const char* part1 =
      "<?xml version='1.0' encoding='UTF-8'?>"
      "<feed xmlns='http://www.w3.org/2005/Atom'>"
      "  <entry>"
      "    <title>Spl";
  const char* part2 =
      "it Title</title>"
      "    <id>urn:split</id>"
      "    <link type='application/atom+xml' href='/split'/>"
      "  </entry>"
      "</feed>";

  OpdsParser parser;
  parser.write(reinterpret_cast<const uint8_t*>(part1), strlen(part1));
  parser.write(reinterpret_cast<const uint8_t*>(part2), strlen(part2));
  parser.flush();

  ASSERT_FALSE(parser.error());
  ASSERT_EQ(parser.getEntries().size(), 1u);
  ASSERT_STREQ(parser.getEntries()[0].title, "Split Title");
}

// --- clear() resets state ---

void testClear() {
  const char* xml =
      "<?xml version='1.0' encoding='UTF-8'?>"
      "<feed xmlns='http://www.w3.org/2005/Atom'>"
      "  <entry>"
      "    <title>Test</title>"
      "    <id>1</id>"
      "    <link type='application/atom+xml' href='/test'/>"
      "  </entry>"
      "</feed>";

  OpdsParser parser;
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  // Don't flush — just check clear resets entries parsed so far
  ASSERT_EQ(parser.getEntries().size(), 1u);
  parser.clear();
  ASSERT_EQ(parser.getEntries().size(), 0u);
}

int main() {
  std::cout << "OpdsParserTest\n";
  RUN_TEST(testNavigationFeed);
  RUN_TEST(testAcquisitionFeed);
  RUN_TEST(testMixedEntries);
  RUN_TEST(testNamespacePrefixes);
  RUN_TEST(testEntryWithoutHref);
  RUN_TEST(testEmptyFeed);
  RUN_TEST(testMalformedXml);
  RUN_TEST(testChunkedWrite);
  RUN_TEST(testClear);
  TEST_SUMMARY();
}

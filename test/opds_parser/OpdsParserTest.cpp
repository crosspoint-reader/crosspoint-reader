#include <OpdsParser.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {
bool parseSingleBookEntry(OpdsEntry& entryOut, const char* href, const char* type = "application/epub+zip") {
  const std::string xml = std::string(R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-1</id>
    <link rel="http://opds-spec.org/acquisition" type=")") +
                          type + R"(" href=")" + href + R"("/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
  parser.flush();

  if (parser.error()) {
    ADD_FAILURE() << "parser.error() returned true";
    return false;
  }
  if (entries.size() != 1) {
    ADD_FAILURE() << "entries.size() == " << entries.size() << ", expected 1";
    return false;
  }
  entryOut = entries.front();
  return true;
}

bool assertSingleFormat(const OpdsEntry& entry, const char* formatKey, const char* fileExtension) {
  if (entry.type != OpdsEntryType::BOOK) {
    ADD_FAILURE() << "entry.type != OpdsEntryType::BOOK";
    return false;
  }
  if (entry.acquisitionLinks.size() != 1) {
    ADD_FAILURE() << "entry.acquisitionLinks.size() == " << entry.acquisitionLinks.size() << ", expected 1";
    return false;
  }
  if (entry.acquisitionLinks[0].formatKey != formatKey) {
    ADD_FAILURE() << "formatKey actual='" << entry.acquisitionLinks[0].formatKey << "' expected='" << formatKey << "'";
    return false;
  }
  if (entry.acquisitionLinks[0].fileExtension != fileExtension) {
    ADD_FAILURE() << "fileExtension actual='" << entry.acquisitionLinks[0].fileExtension << "' expected='"
                  << fileExtension << "'";
    return false;
  }
  return true;
}
}  // namespace

TEST(OpdsParser, EpubExtension) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/books/example.epub"));
  ASSERT_TRUE(assertSingleFormat(entry, "epub", ".epub"));
}

TEST(OpdsParser, KepubDoubleExtension) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/books/example.kepub.epub"));
  ASSERT_TRUE(assertSingleFormat(entry, "kepub", ".kepub.epub"));
}

TEST(OpdsParser, BareKepubExtension) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/books/example.kepub"));
  ASSERT_TRUE(assertSingleFormat(entry, "kepub", ".kepub.epub"));
}

TEST(OpdsParser, SlashTerminatedKepubPath) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/opds/download/6516/kepub/"));
  ASSERT_TRUE(assertSingleFormat(entry, "kepub", ".kepub.epub"));
}

TEST(OpdsParser, SlashTerminatedEpubPath) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/opds/download/6516/epub/"));
  ASSERT_TRUE(assertSingleFormat(entry, "epub", ".epub"));
}

TEST(OpdsParser, DistinctAcquisitionFormatsRemainSeparate) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-2</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.kepub.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="text/plain" href="/books/example.txt"/>
    <link rel="http://opds-spec.org/acquisition" type="text/markdown" href="/books/example.md"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  const auto& links = entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), static_cast<size_t>(4));
  ASSERT_EQ(links[0].formatKey, "epub");
  ASSERT_EQ(links[1].formatKey, "kepub");
  ASSERT_EQ(links[2].formatKey, "txt");
  ASSERT_EQ(links[3].formatKey, "md");
}

TEST(OpdsParser, UnsupportedMimeType) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-3</id>
    <link rel="http://opds-spec.org/acquisition" type="application/x-mobipocket-ebook" href="/books/example.mobi"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(0));
}

TEST(OpdsParser, EmptyHrefOrType) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Empty Href</title>
    <author><name>Example Author</name></author>
    <id>book-4</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href=""/>
  </entry>
  <entry>
    <title>Empty Type</title>
    <author><name>Example Author</name></author>
    <id>book-5</id>
    <link rel="http://opds-spec.org/acquisition" type="" href="/books/example.epub"/>
  </entry>
  <entry>
    <title>Missing Href</title>
    <author><name>Example Author</name></author>
    <id>book-6</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip"/>
  </entry>
  <entry>
    <title>Missing Type</title>
    <author><name>Example Author</name></author>
    <id>book-7</id>
    <link rel="http://opds-spec.org/acquisition" href="/books/example.epub"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(0));
}

TEST(OpdsParser, DuplicateAcquisitionLinks) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-8</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example-copy.epub"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  const auto& links = entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), static_cast<size_t>(2));
  ASSERT_EQ(links[0].formatKey, "epub");
  ASSERT_EQ(links[0].href, "/books/example.epub");
  ASSERT_EQ(links[1].formatKey, "epub");
  ASSERT_EQ(links[1].href, "/books/example-copy.epub");
}

TEST(OpdsParser, IdenticalHrefAcquisitionLinksAreDeduplicated) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-9</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  const auto& links = entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), static_cast<size_t>(1));
  ASSERT_EQ(links[0].formatKey, "epub");
  ASSERT_EQ(links[0].href, "/books/example.epub");
}

TEST(OpdsParser, SlashVariantHrefAcquisitionLinksAreDeduplicated) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-10</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub/"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  const auto& links = entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), static_cast<size_t>(1));
  ASSERT_EQ(links[0].formatKey, "epub");
  ASSERT_EQ(links[0].href, "/books/example.epub");
}

// A translator or illustrator listed after the author must not replace them: the name is shown in
// the list and goes into the download's file name.
TEST(OpdsParser, FirstAuthorIsKept) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Leo Tolstoy</name></author>
    <author><name>Louise Maude</name></author>
    <id>book-11</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  EXPECT_EQ(entries.front().author, "Leo Tolstoy");
}

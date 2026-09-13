#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "OpdsParser.h"

bool operator==(const OpdsEntry& lhs, const OpdsEntry& rhs) {
  return lhs.author == rhs.author && lhs.href == rhs.href && lhs.id == rhs.id && lhs.title == rhs.title &&
         lhs.type == rhs.type;
}
void PrintTo(const OpdsEntry& e, std::ostream* os) {
  const std::string type = e.type == OpdsEntryType::BOOK ? "BOOK" : "NAVIGATION";
  *os << "OpdsEntry{type: " << type << ", title: " << e.title << ", author: " << e.author << ", href: " << e.href
      << ", id: " << e.id << "}";
}

namespace {

/// @brief Defines both a file path and the properties an OpdsParser should have after parsing that path
struct TestFile {
  std::string filename;
  /// @brief If specified, prioritize the provided download format for books
  const char* preferred_format = nullptr;
  bool error = false;
  std::string search_template = "";
  std::string next_page_url = "";
  std::string prev_page_url = "";
  std::vector<OpdsEntry> entries = {};
  std::vector<OpdsEntry> books = {};
};

struct TestFile testFiles[] = {
    {.filename = "test_invalid_file.xml", .error = true},
    {.filename = "test_empty_feed.xml", .error = false},
    {
        .filename = "test_one_of_everything.xml",
        .error = false,
        .search_template = "/search/{searchTerms}.xml",
        .next_page_url = "/page-3.xml",
        .prev_page_url = "/page-1.xml",
        .entries =
            {
                {.type = OpdsEntryType::NAVIGATION, .title = "nav", .author = "", .href = "navhref", .id = "navid"},
                {.type = OpdsEntryType::BOOK, .title = "book", .author = "author", .href = "bookhref", .id = "bookid"},
            },
        .books =
            {{.type = OpdsEntryType::BOOK, .title = "book", .author = "author", .href = "bookhref", .id = "bookid"}},
    },
    {
        .filename = "test_download_priorities.xml",
        .books =
            {
                {.type = OpdsEntryType::BOOK, .title = "onlybook", .href = "onlybook"},
                {.type = OpdsEntryType::BOOK,
                 .title = "Prefers paths containing /epub/",
                 .href = "/epub/c.somethingelse"},
                {.type = OpdsEntryType::BOOK, .title = "Prefers explicit epubs", .href = "d.epub"},
                {.type = OpdsEntryType::BOOK, .title = "Prefers compact epubs", .href = "e.x3.epub"},
                {.type = OpdsEntryType::BOOK, .title = "Ignores order to choose compact epub", .href = "e.x3.epub"},
            },
    },
    {
        .filename = "test_device_download_priorities.xml",
        .preferred_format = OpdsParser::X3_EPUB_EXT,
        .books =
            {
                {.type = OpdsEntryType::BOOK, .title = "Device-specific preferences 1", .href = "a.x3.epub"},
                {.type = OpdsEntryType::BOOK, .title = "Device-specific preferences 2", .href = "b.x3.epub"},
            },
    },
    {
        .filename = "test_device_download_priorities.xml",
        .preferred_format = OpdsParser::X4_EPUB_EXT,
        .books =
            {
                {.type = OpdsEntryType::BOOK, .title = "Device-specific preferences 1", .href = "b.x4.epub"},
                {.type = OpdsEntryType::BOOK, .title = "Device-specific preferences 2", .href = "a.x4.epub"},
            },
    },
};

class OpdsParserTestFixture : public testing::TestWithParam<TestFile> {
 public:
  void SetUp() override {
    const TestFile p = GetParam();

    if (p.preferred_format != nullptr) {
      parser = new OpdsParser(GetParam().preferred_format);
    } else {
      parser = new OpdsParser();
    }

    std::string filepath = std::string(TEST_DATA_DIR) + p.filename;
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      FAIL() << "Failed to open test file " << filepath << std::endl;
      return;
    }
    // Get file size
    std::streamsize size = file.tellg();
    // go back to to the start
    file.seekg(0, std::ios::beg);
    // read the file into a buffer
    char* buffer = new char[size];
    if (!file.read(buffer, size)) {
      FAIL() << "Failed to read test file " << filepath << std::endl;
      delete[] buffer;
      return;
    }
    parser->write(reinterpret_cast<uint8_t*>(buffer), size);
    parser->flush();

    delete[] buffer;
  }
  OpdsParser* parser;
};

TEST_P(OpdsParserTestFixture, Invalid) {
  const TestFile p = GetParam();
  if (p.error) {
    EXPECT_EQ(parser->error(), true);
    return;
  }
  EXPECT_EQ(parser->error(), false);
  EXPECT_EQ(parser->getSearchTemplate(), p.search_template);
  EXPECT_EQ(parser->getNextPageUrl(), p.next_page_url);
  EXPECT_EQ(parser->getPrevPageUrl(), p.prev_page_url);
  if (!p.entries.empty()) {
    // broadly checking entries is redundant with checking books,
    // so only check entries if they are explicitly set
    EXPECT_EQ(parser->getEntries(), p.entries);
  }
  EXPECT_EQ(parser->getBooks(), p.books);
}

INSTANTIATE_TEST_SUITE_P(OpdsParserTest, OpdsParserTestFixture, testing::ValuesIn(testFiles));

}  // namespace

// Host tests for TocNavParser (EPUB 3 nav.xhtml table-of-contents parser).
//
// Regression coverage for issue #2181: the <nav epub:type="landmarks"> section
// (marked hidden="hidden" per the EPUB 3 spec) must not appear in the TOC, even
// when it is nested inside the toc nav.
#include <gtest/gtest.h>

#include <string>

#include "Epub/BookMetadataCache.h"
#include "TocNavParser.h"

namespace {

BookMetadataCache parseNav(const std::string& xml, const std::string& base = "OEBPS/") {
  BookMetadataCache cache;
  TocNavParser parser(base, xml.size(), &cache);
  EXPECT_TRUE(parser.setup());
  parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
  return cache;
}

constexpr const char* kHtmlOpen = "<html xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>";

// Sanity: a plain toc nav yields its entries.
TEST(TocNavParser, CapturesPlainToc) {
  const auto cache = parseNav(std::string(kHtmlOpen) +
                              "<nav epub:type=\"toc\"><ol>"
                              "<li><a href=\"c1.xhtml\">Chapter 1</a></li>"
                              "<li><a href=\"c2.xhtml\">Chapter 2</a></li>"
                              "</ol></nav></body></html>");
  ASSERT_EQ(cache.entries.size(), 2u);
  EXPECT_EQ(cache.entries[0].title, "Chapter 1");
  EXPECT_EQ(cache.entries[1].title, "Chapter 2");
}

// A hidden landmarks nav as a sibling after the toc nav must be excluded.
TEST(TocNavParser, ExcludesHiddenLandmarksSibling) {
  const auto cache = parseNav(std::string(kHtmlOpen) +
                              "<nav epub:type=\"toc\"><ol>"
                              "<li><a href=\"c1.xhtml\">Chapter 1</a></li>"
                              "</ol></nav>"
                              "<nav epub:type=\"landmarks\" hidden=\"hidden\"><ol>"
                              "<li><a href=\"cover.xhtml\">Cover</a></li>"
                              "<li><a href=\"title.xhtml\">Title Page</a></li>"
                              "<li><a href=\"c1.xhtml\">Table of Contents</a></li>"
                              "</ol></nav></body></html>");
  ASSERT_EQ(cache.entries.size(), 1u);
  EXPECT_EQ(cache.entries[0].title, "Chapter 1");
}

// Issue #2181 root cause: a hidden landmarks nav *nested inside* the toc nav.
// Without honoring `hidden`, its entries leaked into the visible TOC.
TEST(TocNavParser, ExcludesHiddenLandmarksNestedInsideToc) {
  const auto cache = parseNav(std::string(kHtmlOpen) +
                              "<nav epub:type=\"toc\"><ol>"
                              "<li><a href=\"c1.xhtml\">Chapter 1</a></li>"
                              "</ol>"
                              "<nav epub:type=\"landmarks\" hidden=\"hidden\"><ol>"
                              "<li><a href=\"cover.xhtml\">Cover</a></li>"
                              "<li><a href=\"title.xhtml\">Title Page</a></li>"
                              "</ol></nav>"
                              "</nav></body></html>");
  ASSERT_EQ(cache.entries.size(), 1u);
  EXPECT_EQ(cache.entries[0].title, "Chapter 1");
}

// `hidden` with an empty value (common in EPUB 3 output) is still hidden.
TEST(TocNavParser, ExcludesLandmarksWithEmptyHiddenValue) {
  const auto cache = parseNav(std::string(kHtmlOpen) +
                              "<nav epub:type=\"toc\"><ol>"
                              "<li><a href=\"c1.xhtml\">Chapter 1</a></li>"
                              "</ol></nav>"
                              "<nav epub:type=\"landmarks\" hidden=\"\"><ol>"
                              "<li><a href=\"cover.xhtml\">Cover</a></li>"
                              "</ol></nav></body></html>");
  ASSERT_EQ(cache.entries.size(), 1u);
  EXPECT_EQ(cache.entries[0].title, "Chapter 1");
}

// Multi-level nesting in the real toc must be preserved with correct levels.
TEST(TocNavParser, PreservesNestedTocLevels) {
  const auto cache = parseNav(std::string(kHtmlOpen) +
                              "<nav epub:type=\"toc\"><ol>"
                              "<li><a href=\"p1.xhtml\">Part 1</a>"
                              "<ol><li><a href=\"c1.xhtml\">Chapter 1</a></li></ol>"
                              "</li>"
                              "<li><a href=\"p2.xhtml\">Part 2</a></li>"
                              "</ol></nav></body></html>");
  ASSERT_EQ(cache.entries.size(), 3u);
  EXPECT_EQ(cache.entries[0].title, "Part 1");
  EXPECT_EQ(cache.entries[0].level, 1u);
  EXPECT_EQ(cache.entries[1].title, "Chapter 1");
  EXPECT_EQ(cache.entries[1].level, 2u);
  EXPECT_EQ(cache.entries[2].title, "Part 2");
  EXPECT_EQ(cache.entries[2].level, 1u);
}

// A single hidden <li> inside the toc is dropped; its siblings are kept.
TEST(TocNavParser, DropsHiddenTocItem) {
  const auto cache = parseNav(std::string(kHtmlOpen) +
                              "<nav epub:type=\"toc\"><ol>"
                              "<li><a href=\"a.xhtml\">Keep A</a></li>"
                              "<li hidden=\"hidden\"><a href=\"b.xhtml\">Hide B</a></li>"
                              "<li><a href=\"c.xhtml\">Keep C</a></li>"
                              "</ol></nav></body></html>");
  ASSERT_EQ(cache.entries.size(), 2u);
  EXPECT_EQ(cache.entries[0].title, "Keep A");
  EXPECT_EQ(cache.entries[1].title, "Keep C");
}

}  // namespace

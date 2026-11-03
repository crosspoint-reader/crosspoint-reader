#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "Print.h"
#include "TxtToHtml.h"

namespace {

class StringPrint : public Print {
 public:
  std::string str;
  size_t write(uint8_t b) override {
    str.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    str.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};

const std::string kHeader =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- TXT_CACHE_VERSION: 1 -->\n"
    "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\n";
const std::string kFooter = "\n</body>\n</html>\n";

std::string convert(std::string_view content) {
  StringPrint out;
  EXPECT_TRUE(TxtToHtml::stream("test.txt", content, out));
  return out.str;
}

TEST(TxtToHtmlTest, LeadingSpacesIndentation) {
  EXPECT_EQ(convert("  two spaces"), kHeader + "&#160;&#160;two spaces" + kFooter);
  EXPECT_EQ(convert("    four spaces"), kHeader + "&#160;&#160;&#160;&#160;four spaces" + kFooter);
  EXPECT_EQ(convert("Line 1\n   three spaces"), kHeader + "Line 1<br />&#160;&#160;&#160;three spaces" + kFooter);
}

TEST(TxtToHtmlTest, MidLineConsecutiveSpaces) {
  EXPECT_EQ(convert("One space"), kHeader + "One space" + kFooter);
  EXPECT_EQ(convert("Two  spaces"), kHeader + "Two&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Three   spaces"), kHeader + "Three&#160;&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Four    spaces"), kHeader + "Four&#160;&#160;&#160; spaces" + kFooter);
}

TEST(TxtToHtmlTest, PreservesCacheVersionTagsForBothFormats) {
  StringPrint out;
  ASSERT_TRUE(TxtToHtml::stream("test.MD", "text", out));
  EXPECT_EQ(out.str,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- MD_CACHE_VERSION: 2 -->\n"
            "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\ntext" +
                kFooter);
  EXPECT_NE(out.str.find(TxtToHtml::cacheVersionTag("test.MD")), std::string::npos);
  EXPECT_NE(convert("text").find(TxtToHtml::cacheVersionTag("test.txt")), std::string::npos);
}

std::string markdownBody(std::string_view content, size_t chunk = 4096) {
  struct Reader {
    std::string_view remaining;
    size_t chunk;
  } reader{content, chunk};
  StringPrint out;
  EXPECT_TRUE(TxtToHtml::stream(
      "test.md", &reader,
      [](void* ctx, uint8_t* buf, size_t size) {
        auto& r = *static_cast<Reader*>(ctx);
        const size_t n = std::min({size, r.chunk, r.remaining.size()});
        r.remaining.copy(reinterpret_cast<char*>(buf), n);
        r.remaining.remove_prefix(n);
        return static_cast<int>(n);
      },
      out));
  const size_t start = out.str.find("<body>\n") + 7;
  return out.str.substr(start, out.str.size() - start - kFooter.size());
}

TEST(TxtToHtmlTest, MarkdownHeadingsAndInlineSpans) {
  EXPECT_EQ(markdownBody("# Title\n## **Bold**\n###### Small\n*italic* and `a < b`"),
            "<h1>Title</h1><h2><b>Bold</b></h2><h6>Small</h6><i>italic</i> and <code>a &lt; b</code>");
  EXPECT_EQ(markdownBody("**bold** & <raw>\n\nlast\n"), "<b>bold</b> &amp; &lt;raw&gt;<br /><br />last<br />");
  EXPECT_EQ(convert("# **literal**"), kHeader + "# **literal**" + kFooter);
}

TEST(TxtToHtmlTest, MarkdownLiteralAndUnsupportedSyntax) {
  EXPECT_EQ(markdownBody("#no heading\n####### too many\n**unclosed\n*spaced *\n| table |"),
            "#no heading<br />####### too many<br />**unclosed<br />*spaced *<br />| table |");
  EXPECT_EQ(markdownBody(""), "");
  EXPECT_EQ(markdownBody("#"), "#");
  EXPECT_EQ(markdownBody("**"), "**");
}

TEST(TxtToHtmlTest, MarkdownReadBoundariesAndLongLineFallback) {
  const std::string text = "\xEF\xBB\xBF# café\r\n**bold** and *italic*\n";
  EXPECT_EQ(markdownBody(text, 7), markdownBody(text));
  const std::string longLine = "# **" + std::string(4096, 'x') + "**<&";
  EXPECT_EQ(markdownBody(longLine + "\n# Next", 13),
            "# **" + std::string(4096, 'x') + "**&lt;&amp;<br /><h1>Next</h1>");
  EXPECT_EQ(markdownBody(std::string(4096, 'x')), std::string(4096, 'x'));
}

TEST(TxtToHtmlTest, MarkdownFlatListsAndQuotes) {
  EXPECT_EQ(
      markdownBody("- **one**\n* two\n+ three\n1. first\n2. second\n> *quote*\nplain", 1),
      "<ul><li><b>one</b></li><li>two</li><li>three</li></ul>"
      "<ol><li>first</li><li>second</li></ol><blockquote style=\"margin-left: 1em\"><i>quote</i></blockquote>plain");
  EXPECT_EQ(markdownBody("1. first\n\n1. restarted\n"), "<ol><li>first</li></ol><br /><ol><li>restarted</li></ol>");
  EXPECT_EQ(markdownBody("-one\n1.no\n  - nested\n>no\n>"), "-one<br />1.no<br />  - nested<br />&gt;no<br />&gt;");
  EXPECT_EQ(markdownBody("- short\n- " + std::string(4096, 'x') + "\n- last"),
            "<ul><li>short</li></ul>- " + std::string(4096, 'x') + "<br /><ul><li>last</li></ul>");
}

TEST(TxtToHtmlTest, MarkdownPropagatesIoFailures) {
  StringPrint out;
  EXPECT_FALSE(TxtToHtml::stream("test.md", nullptr, [](void*, uint8_t*, size_t) { return -1; }, out));
  class FullPrint : public Print {
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t*, size_t) override { return 0; }
  } full;
  EXPECT_FALSE(TxtToHtml::stream("test.md", "# Title", full));
}

}  // namespace

// Host tests for htmlToPlainText(), used to render dictionary definitions that
// cannot be laid out as styled pages.

#include <gtest/gtest.h>

#include <string>

#include "HtmlToPlainText.h"

namespace {

TEST(HtmlToPlainText, StripsTagsAndKeepsText) {
  EXPECT_EQ(htmlToPlainText("<b>quixotic</b> <i>adj.</i>"), "quixotic adj.");
  EXPECT_EQ(htmlToPlainText("<span class=\"x\">in span</span>"), "in span");
  EXPECT_EQ(htmlToPlainText("plain"), "plain");
  EXPECT_EQ(htmlToPlainText(""), "");
}

TEST(HtmlToPlainText, BlockElementsBecomeBreaks) {
  EXPECT_EQ(htmlToPlainText("a<br>b"), "a\nb");
  EXPECT_EQ(htmlToPlainText("<div>a</div><div>b</div>"), "a\nb");
  EXPECT_EQ(htmlToPlainText("<p>a</p><p>b</p>"), "a\n\nb");
  // Consecutive breaks do not stack up.
  EXPECT_EQ(htmlToPlainText("a<br><br><br>b"), "a\nb");
}

TEST(HtmlToPlainText, HeadingsBreakLikeParagraphs) {
  // tagBreak() lists h1-h6 among the paragraph-breaking names, so they must
  // actually reach that comparison -- a name scan that stops at the first digit
  // reads "h1" as "h" and the heading runs into the text after it.
  for (const char* tag : {"h1", "h2", "h3", "h4", "h5", "h6"}) {
    const std::string html = std::string("<") + tag + ">title</" + tag + ">body";
    EXPECT_EQ(htmlToPlainText(html), "title\n\nbody") << tag;
  }
  EXPECT_EQ(htmlToPlainText("<hr>after"), "after");
}

TEST(HtmlToPlainText, DecodesEntities) {
  EXPECT_EQ(htmlToPlainText("Tom &amp; Jerry"), "Tom & Jerry");
  EXPECT_EQ(htmlToPlainText("a&nbsp;b"),
            "a\xC2\xA0"
            "b");
  EXPECT_EQ(htmlToPlainText("&#65;&#66;"), "AB");
  EXPECT_EQ(htmlToPlainText("&#x2014;"), "\xE2\x80\x94");
  // Not entities: left as written rather than swallowed.
  EXPECT_EQ(htmlToPlainText("&notanentity;"), "&notanentity;");
  EXPECT_EQ(htmlToPlainText("100% & up"), "100% & up");
}

TEST(HtmlToPlainText, TrimsSurroundingWhitespace) {
  EXPECT_EQ(htmlToPlainText("<p>only</p>"), "only");
  EXPECT_EQ(htmlToPlainText("text   "), "text");
  EXPECT_EQ(htmlToPlainText("a\tb"), "a b");
  EXPECT_EQ(htmlToPlainText("<br>a"), "a");
}

TEST(HtmlToPlainText, SurvivesMalformedMarkup) {
  EXPECT_EQ(htmlToPlainText("a <b"), "a <b");
  EXPECT_EQ(htmlToPlainText("x < y"), "x < y");
  EXPECT_EQ(htmlToPlainText("<!-- comment -->kept"), "kept");
}

}  // namespace

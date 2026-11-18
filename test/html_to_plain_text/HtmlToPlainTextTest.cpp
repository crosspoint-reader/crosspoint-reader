// Host tests for htmlToPlainText(), used to render dictionary definitions that
// cannot be laid out as styled pages.

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "HtmlToPlainText.h"

namespace {

TEST(HtmlToPlainText, ReusesOwnedBufferForLargeFallback) {
  const std::string text(60 * 1024, 'a');
  std::string html = "<p>" + text + "</p>";
  const char* buffer = html.data();
  const auto output = htmlToPlainText(std::move(html), '\x01');
  EXPECT_EQ(output, text);
  EXPECT_EQ(output.data(), buffer);
}

TEST(HtmlToPlainText, StripsTagsAndKeepsText) {
  EXPECT_EQ(htmlToPlainText("<b>quixotic</b> <i>adj.</i>", '\x01'), "quixotic adj.");
  EXPECT_EQ(htmlToPlainText("<span class=\"x\">in span</span>", '\x01'), "in span");
  EXPECT_EQ(htmlToPlainText("plain", '\x01'), "plain");
  EXPECT_EQ(htmlToPlainText("", '\x01'), "");
}

TEST(HtmlToPlainText, QuotedAttributesCanContainGreaterThan) {
  EXPECT_EQ(htmlToPlainText("<span title=\"a > b\">word</span>", '\x01'), "word");
  EXPECT_EQ(htmlToPlainText("<span title='a > b'>word</span>", '\x01'), "word");
  EXPECT_EQ(htmlToPlainText("<div data-test=\"a > b\">first</div><p>second</p>", '\x01'), "first\n\nsecond");

  const std::string html = "<span title=\"a > b\">word</span>";
  const size_t end = htmlTagEnd(html, 0, html.size());
  EXPECT_EQ(end, html.find(">word"));
  EXPECT_EQ(htmlTagEnd(html, 0, end), std::string::npos);
}

TEST(HtmlToPlainText, CommentsPreserveFollowingVisibleVocabularyText) {
  const std::string html = "<!-- don't -->kept it's</p>";
  EXPECT_EQ(htmlToPlainText(html, '\x01'), "kept it's");
  EXPECT_EQ(htmlTagEnd(html, 0, html.size()), html.find("-->") + 2);
  EXPECT_EQ(htmlTagEnd(html, 0, html.find("-->") + 2), std::string::npos);

  EXPECT_EQ(htmlToPlainText("<!-- 'quoted > text' -->kept", '\x01'), "kept");
  // Non-comment declarations still respect quoted '>' characters.
  EXPECT_EQ(htmlToPlainText("<!DOCTYPE html PUBLIC 'part > value'>kept", '\x01'), "kept");
}

TEST(HtmlToPlainText, BlockElementsBecomeBreaks) {
  EXPECT_EQ(htmlToPlainText("a<br>b", '\x01'), "a\nb");
  EXPECT_EQ(htmlToPlainText("<div>a</div><div>b</div>", '\x01'), "a\nb");
  EXPECT_EQ(htmlToPlainText("<p>a</p><p>b</p>", '\x01'), "a\n\nb");
  // Consecutive breaks do not stack up.
  EXPECT_EQ(htmlToPlainText("a<br><br><br>b", '\x01'), "a\nb");
}

TEST(HtmlToPlainText, HeadingsBecomeBoldLabelLines) {
  // tagBreak() lists h1-h6 among the heading names, so they must actually
  // reach that comparison -- a name scan that stops at the first digit
  // reads "h1" as "h" and the heading runs into the text after it.
  const char marker = '\x01';
  for (const char* tag : {"h1", "h2", "h3", "h4", "h5", "h6"}) {
    const std::string html = std::string("<") + tag + ">title</" + tag + ">body";
    EXPECT_EQ(htmlToPlainText(html, marker), std::string(1, marker) + "title\nbody") << tag;
  }
  // The marker only means something at the start of a line, so a heading that
  // follows text still starts its own line.
  EXPECT_EQ(htmlToPlainText("lead<h2>title</h2>body", marker), "lead\n" + std::string(1, marker) + "title\nbody");
  EXPECT_EQ(htmlToPlainText("<hr>after", marker), "after");
}

TEST(HtmlToPlainText, DecodesEntities) {
  EXPECT_EQ(htmlToPlainText("Tom &amp; Jerry", '\x01'), "Tom & Jerry");
  EXPECT_EQ(htmlToPlainText("a&nbsp;b", '\x01'),
            "a\xC2\xA0"
            "b");
  EXPECT_EQ(htmlToPlainText("&#65;&#66;", '\x01'), "AB");
  EXPECT_EQ(htmlToPlainText("&#x2014;", '\x01'), "\xE2\x80\x94");
  // Not entities: left as written rather than swallowed.
  EXPECT_EQ(htmlToPlainText("&notanentity;", '\x01'), "&notanentity;");
  EXPECT_EQ(htmlToPlainText("100% & up", '\x01'), "100% & up");
  EXPECT_EQ(htmlToPlainText("<p>Literal &lt;b&gt;word&lt;/b&gt; and &#60;tag&#62;</p>", '\x01'),
            "Literal <b>word</b> and <tag>");
}

TEST(HtmlToPlainText, TrimsSurroundingWhitespace) {
  EXPECT_EQ(htmlToPlainText("<p>only</p>", '\x01'), "only");
  EXPECT_EQ(htmlToPlainText("text   ", '\x01'), "text");
  EXPECT_EQ(htmlToPlainText("a\tb", '\x01'), "a b");
  EXPECT_EQ(htmlToPlainText("<br>a", '\x01'), "a");
}

TEST(HtmlToPlainText, SurvivesMalformedMarkup) {
  EXPECT_EQ(htmlToPlainText("a <b", '\x01'), "a <b");
  EXPECT_EQ(htmlToPlainText("x < y", '\x01'), "x < y");
  EXPECT_EQ(htmlToPlainText("<!-- comment -->kept", '\x01'), "kept");
}

}  // namespace

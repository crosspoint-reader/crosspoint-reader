#include <expat.h>
#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <string>

#include "EpubBuilder.h"
#include "JsonStringStreamer.h"
#include "XhtmlSanitizer.h"

using namespace readwise;

namespace {

std::string sanitize(const std::string& in, size_t chunk = 7) {
  std::string out;
  XhtmlSanitizer s([&](const char* d, size_t n) {
    out.append(d, n);
    return true;
  });
  for (size_t i = 0; i < in.size(); i += chunk) s.feed(in.data() + i, std::min(chunk, in.size() - i));
  s.finish();
  return out;
}

bool wellFormed(const std::string& body) {
  XML_Parser p = XML_ParserCreate("UTF-8");
  const std::string doc = "<body>" + body + "</body>";
  const bool ok = XML_Parse(p, doc.data(), static_cast<int>(doc.size()), 1) == XML_STATUS_OK;
  XML_ParserFree(p);
  return ok;
}

}  // namespace

TEST(XhtmlSanitizer, KeepsStructureDropsAttributes) {
  EXPECT_EQ(sanitize("<p class=\"x\" data-a='1>2'>Hi <em>there</em></p>"), "<p>Hi <em>there</em></p>");
}

TEST(XhtmlSanitizer, VoidElementsAreSelfClosed) { EXPECT_EQ(sanitize("a<br>b<hr/>c"), "a<br/>b<hr/>c"); }

TEST(XhtmlSanitizer, DropsImagesScriptsAndSvgSubtrees) {
  EXPECT_EQ(sanitize("<figure><img src=\"x\"><figcaption>Cap</figcaption></figure>"),
            "<figure><figcaption>Cap</figcaption></figure>");
  EXPECT_EQ(sanitize("a<script>if (a<b) {}</script>b<svg><svg/><g>x</g></svg>c<style>p{}</style>d"), "abcd");
  EXPECT_EQ(sanitize("<SCRIPT>x='</scripts>'</Script >y"), "y");
}

TEST(XhtmlSanitizer, UnwrapsUnknownElements) { EXPECT_EQ(sanitize("<a href=\"u\">link</a> <font>f</font>"), "link f"); }

TEST(XhtmlSanitizer, EscapesBareAmpersandsAndAngleBrackets) {
  EXPECT_EQ(sanitize("Q&A 1 < 2 > 0 &copy; &amp; &#38; &#x41; &nope;"),
            "Q&amp;A 1 &lt; 2 &gt; 0 \xC2\xA9 &amp; &amp; A &amp;nope;");
}

TEST(XhtmlSanitizer, BalancesTags) {
  EXPECT_EQ(sanitize("<div><p>a<b>b</div>c</p></i>"), "<div><p>a<b>b</b></p></div>c");
  EXPECT_EQ(sanitize("<ul><li>x"), "<ul><li>x</li></ul>");
}

TEST(XhtmlSanitizer, RepairsInvalidUtf8AndControls) {
  EXPECT_EQ(sanitize("a\x01"
                     "b\xFF"
                     "c\xC3"),
            "ab\xEF\xBF\xBD"
            "c\xEF\xBF\xBD");
  EXPECT_EQ(sanitize("\xED\xA0\x80"), "\xEF\xBF\xBD");  // encoded surrogate
}

TEST(XhtmlSanitizer, DropsCommentsAndDeclarations) {
  EXPECT_EQ(sanitize("<!DOCTYPE html><!-- a -- b -->x<?php ?>y"), "xy");
}

TEST(XhtmlSanitizer, RandomInputIsAlwaysWellFormed) {
  const char* toks[] = {"<p>",
                        "</p>",
                        "<div a=\"<\">",
                        "</div>",
                        "<br>",
                        "<img src='&'>",
                        "<script>",
                        "</script>",
                        "<svg>",
                        "</svg>",
                        "&amp;",
                        "&nbsp;",
                        "&#x1F600;",
                        "&#55357;",
                        "& ",
                        "<",
                        "<!-- -->",
                        "<!X>",
                        "</li>",
                        "<li>",
                        "\xC3",
                        "\xFF",
                        "\x01",
                        "<P>",
                        "</P >",
                        "<x:y>",
                        "text ",
                        "'",
                        "\"",
                        ">",
                        "&gt",
                        "<table><tr><td>",
                        "</table>",
                        "<h1/>",
                        "<averylongtagnamehere>"};
  std::mt19937 rng(42);
  for (int c = 0; c < 2000; ++c) {
    std::string in;
    const int len = static_cast<int>(rng() % 120);
    for (int i = 0; i < len; ++i) {
      if (rng() % 10 == 0) {
        in.push_back(static_cast<char>(rng() % 256));
      } else {
        in += toks[rng() % (sizeof(toks) / sizeof(*toks))];
      }
    }
    const std::string out = sanitize(in, 1 + rng() % 13);
    ASSERT_TRUE(wellFormed(out)) << "input: " << in << "\noutput: " << out;
  }
}

TEST(JsonStringStreamer, DecodesTargetValueAcrossChunks) {
  const std::string json = R"({"count":1,"results":[{"id":"x","summary":"has \"html_content\": inside","title":"té",)"
                           R"("html_content":"<p>a\"b\\c\/d\né😀</p>","other":"z"}]})";
  for (size_t chunk = 1; chunk < 9; ++chunk) {
    std::string got;
    JsonStringStreamer js("html_content", [&](const char* d, size_t n) {
      got.append(d, n);
      return true;
    });
    for (size_t i = 0; i < json.size(); i += chunk) js.feed(json.data() + i, std::min(chunk, json.size() - i));
    EXPECT_TRUE(js.found());
    EXPECT_TRUE(js.complete());
    EXPECT_EQ(got, "<p>a\"b\\c/d\n\xC3\xA9\xF0\x9F\x98\x80</p>");
  }
}

TEST(JsonStringStreamer, NullValueIsNotFound) {
  JsonStringStreamer js("html_content", [](const char*, size_t) { return true; });
  const std::string json = R"({"results":[{"html_content":null}]})";
  js.feed(json.data(), json.size());
  EXPECT_FALSE(js.found());
}

TEST(EpubBuilder, WritesValidStoredZip) {
  std::string zip;
  EpubBuilder b([&](const uint8_t* d, size_t n) {
    zip.append(reinterpret_cast<const char*>(d), n);
    return true;
  });
  ArticleMeta m;
  m.id = "01k";
  m.title = "A & B <C>";
  m.author = "Ann";
  m.siteName = "site";
  m.language = "en";
  const std::string body = "<p>Hello</p>";
  ASSERT_TRUE(b.writeArticle(m, body.size(), [&](const EpubBuilder::Write& w) {
    return w(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  }));
  // mimetype first, stored, uncompressed.
  ASSERT_GT(zip.size(), 60u);
  EXPECT_EQ(zip.substr(0, 4), std::string("PK\x03\x04", 4));
  EXPECT_EQ(zip.substr(30, 8), "mimetype");
  EXPECT_EQ(zip.substr(38, 20), "application/epub+zip");
  // End of central directory with 7 entries.
  const size_t eocd = zip.rfind(std::string("PK\x05\x06", 4));
  ASSERT_NE(eocd, std::string::npos);
  EXPECT_EQ(static_cast<uint8_t>(zip[eocd + 10]), 7);
  // The chapter is well-formed and CRC matches.
  const std::string chapter = EpubBuilder::chapterHead(m) + body + EpubBuilder::chapterTail();
  const size_t at = zip.find(chapter);
  ASSERT_NE(at, std::string::npos);
  uint32_t storedCrc = 0;
  for (int i = 0; i < 4; ++i)
    storedCrc |= static_cast<uint32_t>(static_cast<uint8_t>(zip[at - 19 - 30 + 14 + i])) << (8 * i);
  EXPECT_EQ(storedCrc, crc32Update(0, reinterpret_cast<const uint8_t*>(chapter.data()), chapter.size()));
  XML_Parser p = XML_ParserCreate("UTF-8");
  EXPECT_EQ(XML_Parse(p, chapter.data(), static_cast<int>(chapter.size()), 1), XML_STATUS_OK);
  XML_ParserFree(p);
}

TEST(LanguageGuesser, PicksDutchAndFallsBackToEnglish) {
  LanguageGuesser nl;
  const std::string dutch =
      "<p>Het is niet zo dat een kat voor de deur zit, maar het wordt ook niet beter als deze naar huis gaat. "
      "Het kan zijn dat het niet werkt voor een tijd, maar ook dat het wordt opgelost.</p>";
  nl.feed(dutch.data(), dutch.size());
  EXPECT_STREQ(nl.result(), "nl");
  LanguageGuesser none;
  none.feed("<p>x</p>", 8);
  EXPECT_STREQ(none.result(), "en");
}

// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// The cleaner's one hard promise is that expat (built exactly as in the
// firmware) accepts its output, whatever the input. These tests check that on
// every HTML entry of openZIM's real Wikipedia sample, on truncated and
// mangled copies of it, and on hand-written edge cases; and that the reading
// content survives while the chrome does not.

#include <expat.h>
#include <gtest/gtest.h>

#include <random>
#include <string>

#include "PosixSource.h"
#include "ZimArchive.h"
#include "ZimHtml.h"

namespace {

const std::string kDataDir = ZIM_TEST_DATA_DIR;
const std::string kWikipedia = kDataDir + "/withns/wikipedia_en_climate_change_mini_2024-06.zim";

struct ParseResult {
  bool ok = false;
  std::string error;
  std::string text;      // all character data, concatenated
  std::string elements;  // "p,b,/b,/p,..." start/end sequence
  int elementCount = 0;
};

ParseResult parseXml(const std::string& xml) {
  ParseResult r;
  XML_Parser p = XML_ParserCreate("UTF-8");
  XML_SetUserData(p, &r);
  XML_SetElementHandler(
      p,
      [](void* ud, const XML_Char* name, const XML_Char**) {
        auto* res = static_cast<ParseResult*>(ud);
        res->elements += name;
        res->elements += ',';
        res->elementCount++;
      },
      [](void* ud, const XML_Char* name) {
        auto* res = static_cast<ParseResult*>(ud);
        res->elements += '/';
        res->elements += name;
        res->elements += ',';
      });
  XML_SetCharacterDataHandler(p, [](void* ud, const XML_Char* s, int len) {
    static_cast<ParseResult*>(ud)->text.append(s, static_cast<size_t>(len));
  });
  r.ok = XML_Parse(p, xml.data(), static_cast<int>(xml.size()), 1) == XML_STATUS_OK;
  if (!r.ok) {
    r.error =
        std::string(XML_ErrorString(XML_GetErrorCode(p))) + " at line " + std::to_string(XML_GetCurrentLineNumber(p));
  }
  XML_ParserFree(p);
  return r;
}

std::string clean(std::string_view html, bool keepLinks = false, std::string_view title = "T") {
  zim::StringHtmlSink sink;
  zim::HtmlCleanOptions opt;
  opt.title = title;
  opt.keepLinks = keepLinks;
  EXPECT_TRUE(zim::cleanArticleHtml(html, opt, sink));
  return sink.out;
}

// Body content only, without the fixed wrapper.
std::string body(const std::string& xhtml) {
  const size_t a = xhtml.find("<body>");
  const size_t b = xhtml.rfind("</body>");
  if (a == std::string::npos || b == std::string::npos) return xhtml;
  std::string s = xhtml.substr(a + 6, b - a - 6);
  std::string out;
  for (char c : s)
    if (c != '\n') out.push_back(c);
  return out;
}

class Sample : public ::testing::Test {
 protected:
  void SetUp() override {
    auto src = zim::PosixSource::open(kWikipedia);
    ASSERT_TRUE(src) << kWikipedia;
    ASSERT_EQ(archive.open(std::move(src)), zim::Error::None);
  }
  std::string article(std::string_view title) {
    zim::Entry e;
    EXPECT_EQ(archive.findByTitle(archive.contentNamespace(), title, e), zim::Error::None) << title;
    EXPECT_EQ(archive.resolve(e), zim::Error::None);
    std::string html;
    EXPECT_EQ(archive.read(e, html), zim::Error::None);
    return html;
  }
  zim::Archive archive;
};

TEST_F(Sample, RealArticleKeepsTextDropsChrome) {
  const std::string html = article("Climate change");
  zim::HtmlCleanStats stats;
  zim::StringHtmlSink sink;
  zim::HtmlCleanOptions opt;
  opt.title = "Climate change";
  ASSERT_TRUE(zim::cleanArticleHtml(html, opt, sink, &stats));
  const ParseResult r = parseXml(sink.out);
  ASSERT_TRUE(r.ok) << r.error;

  EXPECT_NE(r.text.find("In common usage, climate change describes global warming"), std::string::npos);
  EXPECT_NE(r.elements.find("h1,"), std::string::npos);
  EXPECT_NE(r.elements.find("p,"), std::string::npos);
  EXPECT_NE(r.elements.find("b,"), std::string::npos);
  // Scripts, styles, stylesheet links and the TOC's inline CSS are gone.
  EXPECT_EQ(sink.out.find("script"), std::string::npos);
  EXPECT_EQ(sink.out.find(".js"), std::string::npos);
  EXPECT_EQ(sink.out.find("toclevel"), std::string::npos);
  EXPECT_EQ(sink.out.find("<a"), std::string::npos);  // links off by default
  // Much smaller than the input.
  EXPECT_LT(stats.outputBytes, stats.inputBytes);
  EXPECT_GT(stats.subtreesDropped, 10u);
  EXPECT_EQ(stats.outputBytes, sink.out.size());
}

TEST_F(Sample, EveryHtmlEntryIsWellFormed) {
  int checked = 0;
  for (uint32_t i = 0; i < archive.entryCount(); i++) {
    zim::Entry e;
    ASSERT_EQ(archive.entryAt(i, e), zim::Error::None);
    if (!e.isContent()) continue;
    if (archive.mimeType(e.mime).rfind("text/html", 0) != 0) continue;
    std::string html;
    ASSERT_EQ(archive.read(e, html), zim::Error::None);
    for (bool links : {false, true}) {
      const std::string out = clean(html, links, e.title);
      const ParseResult r = parseXml(out);
      ASSERT_TRUE(r.ok) << e.path << " (links " << links << "): " << r.error;
    }
    checked++;
  }
  EXPECT_GT(checked, 5);
}

TEST_F(Sample, TruncatedAndMangledArticlesStayWellFormed) {
  const std::string html = article("Climate change");
  for (size_t cut = 0; cut < html.size(); cut += 97) {
    const ParseResult r = parseXml(clean(std::string_view(html).substr(0, cut)));
    ASSERT_TRUE(r.ok) << "cut at " << cut << ": " << r.error;
  }
  std::mt19937 rng(42);
  for (int round = 0; round < 200; round++) {
    std::string m = html;
    for (int k = 0; k < 40; k++) m[rng() % m.size()] = static_cast<char>(rng() & 0xFF);
    const ParseResult r = parseXml(clean(m, round % 2 == 0));
    ASSERT_TRUE(r.ok) << "round " << round << ": " << r.error;
  }
}

TEST(HtmlClean, ImpliedEndTags) {
  EXPECT_EQ(body(clean("<p>a<p>b<ul><li>x<li>y</ul><p>c")), "<p>a</p><p>b</p><ul><li>x</li><li>y</li></ul><p>c</p>");
  EXPECT_EQ(body(clean("<dl><dt>term<dd>def<dt>t2</dl>")),
            "<div><p>term</p><blockquote>def</blockquote><p>t2</p></div>");
  EXPECT_EQ(body(clean("<table><tr><td>1<td>2<tr><th>3</table>")),
            "<table><tr><td>1</td><td>2</td></tr><tr><th>3</th></tr></table>");
  // A stray end tag closes nothing it shouldn't.
  EXPECT_EQ(body(clean("<div><b>x</i></b></p></div>")), "<div><b>x</b></div>");
  // An unclosed inline element is closed with its parent.
  EXPECT_EQ(body(clean("<p><b>bold<i>both</p>after")), "<p><b>bold<i>both</i></b></p>after");
}

TEST(HtmlClean, EntitiesAndCharacters) {
  EXPECT_EQ(body(clean("a&nbsp;b&mdash;&#233;&#xE9;&#X1F600;&amp;&lt;&gt;&quot;")),
            "a b—éé\U0001F600&amp;&lt;&gt;&quot;");
  EXPECT_EQ(body(clean("AT&T &bogus; & x &#; &#xZZ;")), "AT&amp;T &amp;bogus; &amp; x &amp;#; &amp;#xZZ;");
  EXPECT_EQ(body(clean("&#150;&#x80;")), "–€");  // Windows-1252 references
  EXPECT_EQ(body(clean("&#0;&#x110000;")), "��");
  // Bad UTF-8 becomes U+FFFD; control characters XML forbids disappear.
  EXPECT_EQ(body(clean(std::string("a\xC3(b\xFF") + "c\x01\x1F" + "d\xE2\x82")), "a�(b�cd��");
  EXPECT_EQ(zim::decodeHtmlEntity("eacute"), 0xE9u);
  EXPECT_EQ(zim::decodeHtmlEntity("AElig"), 0xC6u);
  EXPECT_EQ(zim::decodeHtmlEntity("zwnj"), 0x200Cu);
  EXPECT_EQ(zim::decodeHtmlEntity("nope"), 0u);
}

TEST(HtmlClean, DropsChromeAndHiddenContent) {
  const std::string in =
      "<h2 id=\"History\" class=\"x\">History<span class=\"mw-editsection\">[edit]</span></h2>"
      "<p>Fact.<sup class=\"reference\"><a href=\"#cite_note-1\">[1]</a></sup> More.</p>"
      "<div style=\"display: none\">secret</div><div STYLE=\"DISPLAY:NONE\">s2</div>"
      "<div role=\"navigation\" class=\"navbox\">nav</div><table class=\"navbox\"><tr><td>n</td></tr></table>"
      "<figure><img src=\"a.png\"/><figcaption>cap</figcaption></figure>"
      "<div class=\"thumb tright\"><div class=\"thumbcaption\">thumb</div></div>"
      "<script>if (a < b) { document.write('</div>'); }</script><style>p{}</style>"
      "<noscript>ns</noscript><!-- comment <p>no</p> -->";
  EXPECT_EQ(body(clean(in)), "<h2 id=\"History\">History</h2><p>Fact. More.</p>");
}

TEST(HtmlClean, KeepsStructureAndRenames) {
  EXPECT_EQ(body(clean("<section><h3>S</h3><p><strong>s</strong><em>e</em><cite>c</cite><del>d</del>"
                       "<ins>i</ins>H<sub>2</sub>O x<sup>2</sup><span>sp</span><small>sm</small></p>"
                       "<blockquote>q</blockquote><pre>a  b</pre><hr><br/><br></section>")),
            "<div><h3>S</h3><p><b>s</b><i>e</i><i>c</i><s>d</s><u>i</u>H<sub>2</sub>O x<sup>2</sup>spsm</p>"
            "<blockquote>q</blockquote><div>a  b</div><hr/><br/><br/></div>");
  EXPECT_EQ(body(clean("<table><caption>Cap</caption><tbody><tr><th colspan=\"2\" style=\"x\">h</th></tr>"
                       "<tr><td rowspan=3 class=c>v</td></tr></tbody></table>")),
            "<table><p>Cap</p><tr><th colspan=\"2\">h</th></tr><tr><td rowspan=\"3\">v</td></tr></table>");
  EXPECT_EQ(body(clean("<p dir=\"rtl\">ש</p><p dir=\"evil\">x</p>")), "<p dir=\"rtl\">ש</p><p>x</p>");
}

TEST(HtmlClean, Links) {
  const std::string in =
      "<a href=\"Forbidden_City\">FC</a> <a href=\"https://example.org\">ext</a> "
      "<a href=\"#Notes\">self</a> <a href=\"../C/Beijing#History\" title=\"t\">B&amp;J</a> <a>bare</a>";
  EXPECT_EQ(body(clean(in, false)), "FC ext self B&amp;J bare");
  EXPECT_EQ(body(clean(in, true)),
            "<a href=\"Forbidden_City\">FC</a> ext self <a href=\"../C/Beijing#History\">B&amp;J</a> bare");
}

TEST(HtmlClean, MathShowsItsTeX) {
  EXPECT_EQ(body(clean("E = <math alttext=\"{\\displaystyle mc^{2}}\"><mi>m</mi></math>.")), "E = <i>mc^{2}</i>.");
  EXPECT_EQ(body(clean("<math alttext=\"a&lt;b\"><mi>a</mi></math>")), "<i>a&lt;b</i>");
}

TEST(HtmlClean, MessyMarkup) {
  // Unquoted, single-quoted and valueless attributes; uppercase tags; '<'
  // that starts no tag; unterminated tag at end of input.
  EXPECT_EQ(body(clean("<P CLASS=a ID='b' hidden>x < y <3</P><B>t</b><p")), "<p>x &lt; y &lt;3</p><b>t</b>");
  EXPECT_EQ(body(clean("<head><title>skip</title></head><body><p>in</p></body>after")), "<p>in</p>after");
  EXPECT_EQ(body(clean("<style>never closed")), "");
  EXPECT_EQ(body(clean("<div class=\"navbox\"><div><p>deep</p></div></div><p>out</p>")), "<p>out</p>");
  // Title is escaped.
  EXPECT_NE(clean("", false, "A & B <C>").find("<title>A &amp; B &lt;C&gt;</title>"), std::string::npos);
}

TEST(HtmlClean, SinkFailureStops) {
  struct Refuse final : zim::HtmlSink {
    bool write(const char*, size_t) override { return false; }
  } sink;
  zim::HtmlCleanOptions opt;
  EXPECT_FALSE(zim::cleanArticleHtml("<p>x</p>", opt, sink));
}

TEST(HtmlClean, LargeInputStreamsInChunks) {
  struct Count final : zim::HtmlSink {
    bool write(const char*, size_t len) override {
      writes++;
      biggest = std::max(biggest, len);
      total += len;
      return true;
    }
    int writes = 0;
    size_t biggest = 0, total = 0;
  } sink;
  std::string in;
  for (int i = 0; i < 20000; i++) in += "<p>Paragraph number " + std::to_string(i) + " with some text.</p>";
  zim::HtmlCleanOptions opt;
  ASSERT_TRUE(zim::cleanArticleHtml(in, opt, sink));
  EXPECT_GT(sink.writes, 100);
  EXPECT_LT(sink.biggest, 8192u);
  EXPECT_GT(sink.total, in.size() / 2);
}

}  // namespace

namespace {
std::string leadOf(std::string_view html, size_t limit = 600) {
  zim::StringHtmlSink sink;
  std::string lead;
  zim::HtmlCleanOptions o;
  o.lead = &lead;
  o.leadLimit = limit;
  EXPECT_TRUE(zim::cleanArticleHtml(html, o, sink));
  return lead;
}
}  // namespace

TEST(HtmlClean, LeadIsTheOpeningProse) {
  const std::string lead = leadOf(
      "<h1>Panda</h1><table class=infobox><tr><td><p>Infobox text</p></td></tr></table>"
      "<p class=mw-empty-elt></p><p>The <b>giant panda</b> is a bear.\n It eats   bamboo.</p>"
      "<ul><li>a list</li></ul><p>Second paragraph.</p><h2>History</h2><p>Later text.</p>");
  EXPECT_EQ(lead, "The giant panda is a bear. It eats bamboo. Second paragraph.");
}

TEST(HtmlClean, LeadIsCutAtAWord) {
  std::string body = "<p>";
  for (int i = 0; i < 100; i++) body += "word" + std::to_string(i) + " ";
  body += "</p>";
  const std::string lead = leadOf(body, 50);
  EXPECT_LE(lead.size(), 53u);
  EXPECT_EQ(lead.compare(lead.size() - 3, 3, "\xE2\x80\xA6"), 0) << lead;
  EXPECT_EQ(lead.find("  "), std::string::npos);
}

TEST(HtmlClean, FirstSentences) {
  EXPECT_EQ(zim::firstSentences("The U.S. Army is big. It has tanks. It is old.", 2, 200),
            "The U.S. Army is big. It has tanks.");
  EXPECT_EQ(zim::firstSentences("Paris (c. 250 BC) is a city. More.", 1, 200), "Paris (c. 250 BC) is a city.");
  EXPECT_EQ(zim::firstSentences("No stop at all", 2, 200), "No stop at all");
}

TEST_F(Sample, RealLeadStartsWithTheArticle) {
  const std::string html = article("Climate change");
  zim::StringHtmlSink sink;
  std::string lead;
  zim::HtmlCleanOptions o;
  o.lead = &lead;
  ASSERT_TRUE(zim::cleanArticleHtml(html, o, sink));
  EXPECT_EQ(lead.rfind("In common usage, climate change describes global warming", 0), 0u) << lead;
  EXPECT_EQ(lead.find('{'), std::string::npos);
  EXPECT_LE(lead.size(), 603u);
}

namespace {
const char* kPictures =
    "<h1>Panda</h1><table class=\"infobox\"><tr><td class=\"infobox-image\">"
    "<a href=\"./File:P.jpg\"><img src=\"../I/P.jpg.webp\" width=\"220\" height=\"330\" alt=\"A &amp; panda\"></a>"
    "<div class=\"infobox-caption\">Lead caption</div></td></tr></table>"
    "<p>Text <img src=\"./I/flag.svg.png.webp\" width=\"20\" height=\"14\"> here.</p>"
    "<figure typeof=\"mw:File/Thumb\"><a href=\"./File:Q.jpg\"><img src=\"./I/Q.jpg.webp\" width=\"200\" "
    "height=\"150\"></a><figcaption>Second caption</figcaption></figure>"
    "<h2>Range</h2><div class=\"thumb tright\"><div class=\"thumbinner\"><img src=\"R.png.webp\" width=\"300\" "
    "height=\"200\"><div class=\"thumbcaption\">Third caption</div></div></div>"
    "<p><img class=\"mwe-math-fallback-image-inline\" src=\"m.svg\" width=\"90\" height=\"40\"></p>";

std::string cleanPictures(zim::HtmlImages mode, std::vector<zim::HtmlImage>& images) {
  zim::StringHtmlSink sink;
  zim::HtmlCleanOptions o;
  o.images = mode;
  o.imageList = &images;
  EXPECT_TRUE(zim::cleanArticleHtml(kPictures, o, sink));
  EXPECT_TRUE(parseXml(sink.out).ok) << sink.out;
  return sink.out;
}
}  // namespace

TEST(HtmlClean, NoImagesByDefault) {
  std::vector<zim::HtmlImage> images;
  const std::string out = cleanPictures(zim::HtmlImages::None, images);
  EXPECT_TRUE(images.empty());
  EXPECT_EQ(out.find("<img"), std::string::npos);
  EXPECT_EQ(out.find("caption"), std::string::npos) << "frames go with their pictures";
}

TEST(HtmlClean, LeadImageOnly) {
  std::vector<zim::HtmlImage> images;
  const std::string out = cleanPictures(zim::HtmlImages::Lead, images);
  ASSERT_EQ(images.size(), 1u);
  EXPECT_EQ(images[0].src, "../I/P.jpg.webp");
  EXPECT_EQ(images[0].width, 220);
  EXPECT_EQ(images[0].height, 330);
  EXPECT_NE(out.find("<img src=\"/pl-img/0.png\" width=\"220\" height=\"330\" alt=\"A &amp; panda\"/>"),
            std::string::npos)
      << out;
  EXPECT_NE(out.find("Lead caption"), std::string::npos);
  EXPECT_EQ(out.find("Second caption"), std::string::npos);
  EXPECT_EQ(out.find("Third caption"), std::string::npos);
}

TEST(HtmlClean, AllImages) {
  std::vector<zim::HtmlImage> images;
  const std::string out = cleanPictures(zim::HtmlImages::All, images);
  ASSERT_EQ(images.size(), 3u) << "the flag icon and the formula stay out";
  EXPECT_EQ(images[1].src, "./I/Q.jpg.webp");
  EXPECT_EQ(images[2].src, "R.png.webp");
  EXPECT_NE(out.find("/pl-img/2.png"), std::string::npos);
  EXPECT_NE(out.find("Second caption"), std::string::npos);
  EXPECT_NE(out.find("Third caption"), std::string::npos);
}

TEST_F(Sample, EveryArticleWithAllImagesIsWellFormed) {
  for (uint32_t i = 0; i < archive.entryCount(); i++) {
    zim::Entry e;
    ASSERT_EQ(archive.entryAt(i, e), zim::Error::None);
    if (!e.isContent() || archive.mimeType(e.mime).rfind("text/html", 0) != 0) continue;
    std::string html;
    ASSERT_EQ(archive.read(e, html), zim::Error::None);
    zim::StringHtmlSink sink;
    std::vector<zim::HtmlImage> images;
    zim::HtmlCleanOptions o;
    o.keepLinks = true;
    o.images = zim::HtmlImages::All;
    o.imageList = &images;
    ASSERT_TRUE(zim::cleanArticleHtml(html, o, sink));
    ASSERT_TRUE(parseXml(sink.out).ok) << e.path;
  }
}

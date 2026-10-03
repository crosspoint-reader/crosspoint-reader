// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Following links: href parsing by hand, then every internal link of every
// article in openZIM's real Wikipedia sample, in both namespace schemes.

#include <gtest/gtest.h>

#include <string>

#include "PosixSource.h"
#include "ZimArchive.h"
#include "ZimHtml.h"
#include "ZimLink.h"

namespace {

const std::string kData = ZIM_TEST_DATA_DIR;

zim::Entry entry(char ns, const std::string& path) {
  zim::Entry e;
  e.ns = ns;
  e.path = path;
  return e;
}

TEST(Link, Parse) {
  zim::LinkTarget t;
  ASSERT_TRUE(zim::parseLink(entry('C', "Climate_change"), "Climate_system", t));
  EXPECT_EQ(t.ns, 'C');
  EXPECT_EQ(t.path, "Climate_system");
  EXPECT_TRUE(t.fragment.empty());

  ASSERT_TRUE(zim::parseLink(entry('C', "Climate_change"), "./Earth%27s_energy_budget#Outgoing", t));
  EXPECT_EQ(t.path, "Earth's_energy_budget");
  EXPECT_EQ(t.fragment, "Outgoing");

  ASSERT_TRUE(zim::parseLink(entry('C', "Book/Chapter_1"), "Chapter_2", t));
  EXPECT_EQ(t.path, "Book/Chapter_2");
  ASSERT_TRUE(zim::parseLink(entry('C', "Book/Part/Ch"), "../Intro", t));
  EXPECT_EQ(t.path, "Book/Intro");

  // Old scheme: climbing out of the namespace names another one.
  ASSERT_TRUE(zim::parseLink(entry('A', "Foo"), "../A/Bar_baz", t));
  EXPECT_EQ(t.ns, 'A');
  EXPECT_EQ(t.path, "Bar_baz");
  ASSERT_TRUE(zim::parseLink(entry('A', "Foo"), "../I/m/x.png", t));
  EXPECT_EQ(t.ns, 'I');
  EXPECT_EQ(t.path, "m/x.png");
  ASSERT_TRUE(zim::parseLink(entry('A', "Foo"), "/C/Abs?x=1#f", t));
  EXPECT_EQ(t.ns, 'C');
  EXPECT_EQ(t.path, "Abs");
  EXPECT_EQ(t.fragment, "f");

  ASSERT_TRUE(zim::parseLink(entry('C', "Foo"), "#Notes", t));
  EXPECT_TRUE(t.samePage);
  EXPECT_EQ(t.path, "Foo");
  EXPECT_EQ(t.fragment, "Notes");

  ASSERT_TRUE(zim::parseLink(entry('C', "Foo"), "AT&amp;T", t));
  EXPECT_EQ(t.path, "AT&T");

  for (const char* ext :
       {"https://en.wikipedia.org/wiki/X", "http://x", "//cdn.x/y", "mailto:a@b", "geo:1,2", "javascript:void(0)"}) {
    EXPECT_TRUE(zim::isExternalHref(ext)) << ext;
    EXPECT_FALSE(zim::parseLink(entry('C', "Foo"), ext, t)) << ext;
  }
  EXPECT_FALSE(zim::parseLink(entry('C', "Foo"), "", t));
  EXPECT_FALSE(zim::parseLink(entry('C', "Foo"), "#", t));
}

class RealLinks : public ::testing::TestWithParam<std::string> {};
INSTANTIATE_TEST_SUITE_P(Wikipedia, RealLinks, ::testing::Values("withns", "nons"));

TEST_P(RealLinks, InternalLinksResolve) {
  zim::Archive a;
  ASSERT_EQ(a.open(zim::openArchiveSource(kData + "/" + GetParam() + "/wikipedia_en_climate_change_mini_2024-06.zim")),
            zim::Error::None);
  size_t links = 0, resolved = 0, external = 0;
  bool sawClimateSystem = false;
  for (uint32_t i = 0; i < a.entryCount(); i++) {
    zim::Entry from;
    ASSERT_EQ(a.entryAt(i, from), zim::Error::None);
    if (!from.isContent() || from.ns != a.contentNamespace()) continue;
    if (a.mimeType(from.mime).rfind("text/html", 0) != 0) continue;
    std::string html;
    ASSERT_EQ(a.read(from, html), zim::Error::None);
    for (size_t p = html.find(" href=\""); p != std::string::npos; p = html.find(" href=\"", p + 1)) {
      const size_t start = p + 7;
      const size_t end = html.find('"', start);
      if (end == std::string::npos) break;
      const std::string href = html.substr(start, end - start);
      // Only links (<a ...>), not stylesheets (<link href>).
      const size_t open = html.rfind('<', p);
      if (open == std::string::npos || html.compare(open, 3, "<a ") != 0) continue;
      if (zim::isExternalHref(href)) {
        external++;
        continue;
      }
      if (href.empty() || href[0] == '#') continue;
      links++;
      zim::Entry to;
      std::string fragment;
      if (zim::resolveLink(a, from, href, to, fragment) == zim::Error::None) {
        resolved++;
        EXPECT_TRUE(to.isContent()) << href;
        if (from.title == "Climate change" && to.title == "Climate system") sawClimateSystem = true;
      }
    }
  }
  // The mini file keeps a few hundred climate articles of Wikipedia's millions,
  // so most links point outside it; the ones inside must resolve.
  EXPECT_GT(links, 500u);
  EXPECT_GT(resolved, 100u);
  EXPECT_GT(external, 0u);
  EXPECT_TRUE(sawClimateSystem);
}

TEST(Headings, CollectedWithAnchorsAndAliases) {
  std::vector<zim::HtmlHeading> headings;
  zim::StringHtmlSink sink;
  zim::HtmlCleanOptions opt;
  opt.headings = &headings;
  ASSERT_TRUE(zim::cleanArticleHtml(
      "<h1>Title</h1><p>a</p>"
      "<h2><span class=\"mw-headline\" id=\"History\">History</span><span class=\"mw-editsection\">[edit]</span></h2>"
      "<p>b</p><h3 id=\"Early_life\">  Early &amp;\n  <i>late</i> life </h3><div class=\"navbox\"><h2>Hidden</h2></div>"
      "<h4>Last",
      opt, sink));
  ASSERT_EQ(headings.size(), 4u);
  EXPECT_EQ(headings[0].level, 1);
  EXPECT_EQ(headings[0].text, "Title");
  EXPECT_EQ(headings[1].level, 2);
  EXPECT_EQ(headings[1].text, "History");
  ASSERT_EQ(headings[1].aliases.size(), 1u);
  EXPECT_EQ(headings[1].aliases[0], "History");
  EXPECT_EQ(headings[2].text, "Early & late life");
  ASSERT_EQ(headings[2].aliases.size(), 1u);
  EXPECT_EQ(headings[2].aliases[0], "Early_life");
  EXPECT_EQ(headings[3].text, "Last");  // unterminated input still closes it
  for (size_t i = 0; i < headings.size(); i++) {
    EXPECT_NE(sink.out.find("id=\"" + zim::headingAnchor(i) + "\""), std::string::npos) << i;
  }
  EXPECT_EQ(sink.out.find("id=\"History\""), std::string::npos);  // replaced by pl-h1
  EXPECT_EQ(sink.out.find("Hidden"), std::string::npos);
}

}  // namespace

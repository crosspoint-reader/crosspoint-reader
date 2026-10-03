// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Every HTML article in openZIM's real Wikipedia sample, cleaned by
// zim::cleanArticleHtml and laid out by ChapterHtmlSlimParser with the same
// arguments ArticleActivity passes on the device (no Epub, no CSS, images
// off). Checks that layout never fails, that the article's words reach the
// pages in order, and that pages survive the write/read round trip through
// the page file.

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "PosixSource.h"
#include "ZimArchive.h"
#include "ZimHtml.h"

namespace {

const std::string kNewNs = std::string(ZIM_TEST_DATA_DIR) + "/nons/wikipedia_en_climate_change_mini_2024-06.zim";
const std::string kOldNs = std::string(ZIM_TEST_DATA_DIR) + "/withns/wikipedia_en_climate_change_mini_2024-06.zim";

class FileSink final : public zim::HtmlSink {
 public:
  explicit FileSink(HalFile& f) : f_(f) {}
  bool write(const char* data, size_t len) override { return f_.write(data, len) == len; }

 private:
  HalFile& f_;
};

struct Layout {
  bool ok = false;
  size_t links = 0;
  std::vector<std::pair<std::string, uint16_t>> anchors;
  std::vector<zim::HtmlHeading> headings;
  std::vector<std::string> words;  // every word on every page, in order
  size_t pages = 0;
  size_t roundTripWords = 0;
};

// Unique per process: ctest runs each test case as its own process, in parallel.
std::string tmpPath(const char* name) {
  return (std::filesystem::temp_directory_path() / (std::to_string(getpid()) + "-" + name)).string();
}

Layout layOut(const std::string& html, const std::string& title, bool readerOptions = false) {
  Layout result;
  const std::string xhtml = tmpPath("pocketlib-article.xhtml");
  const std::string pagesPath = tmpPath("pocketlib-article.pages");
  {
    HalFile out;
    EXPECT_TRUE(out.open(xhtml.c_str(), "wb"));
    FileSink sink(out);
    zim::HtmlCleanOptions options;
    options.title = title;
    if (readerOptions) {  // as ArticleActivity sets them since Milestone 5
      options.keepLinks = true;
      options.headings = &result.headings;
    }
    EXPECT_TRUE(zim::cleanArticleHtml(html, options, sink));
  }

  GfxRenderer renderer;
  HalFile pagesFile;
  EXPECT_TRUE(pagesFile.open(pagesPath.c_str(), "wb"));
  static const std::string kNoBase;
  ChapterHtmlSlimParser parser(
      nullptr, xhtml, renderer, 0, 1.0f, false, 0, 440, 740, false, false,
      [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
        result.pages++;
        result.links += page->links.size();
        for (const auto& el : page->elements) {
          if (el->getTag() != TAG_PageLine) continue;
          const auto& block = *static_cast<const PageLine&>(*el).getBlock();
          for (uint16_t w = 0; w < block.wordCount(); w++) result.words.emplace_back(block.wordText(w));
        }
        EXPECT_TRUE(page->serialize(pagesFile));
      },
      /*embeddedStyle=*/false, kNoBase, kNoBase, /*imageRendering=*/1);

  if (!parser.beginParse()) return result;
  for (;;) {
    const auto status = parser.parseStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      parser.abortParse();
      return result;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) break;
  }
  result.ok = parser.finishParse();
  result.anchors = parser.getAnchors();
  pagesFile.close();

  HalFile in;
  EXPECT_TRUE(in.open(pagesPath.c_str(), "rb"));
  for (size_t i = 0; i < result.pages; i++) {
    auto page = Page::deserialize(in);
    EXPECT_NE(page, nullptr) << "page " << i;
    if (!page) break;
    for (const auto& el : page->elements) {
      if (el->getTag() == TAG_PageLine)
        result.roundTripWords += static_cast<const PageLine&>(*el).getBlock()->wordCount();
    }
  }
  return result;
}

std::string joined(const std::vector<std::string>& words, size_t limit) {
  std::string s;
  for (size_t i = 0; i < words.size() && i < limit; i++) {
    if (i) s += ' ';
    s += words[i];
  }
  return s;
}

class ArticleLayout : public ::testing::TestWithParam<std::string> {};

TEST_P(ArticleLayout, EveryArticleLaysOut) {
  zim::Archive archive;
  auto src = zim::PosixSource::open(GetParam());
  ASSERT_TRUE(src) << GetParam();
  ASSERT_EQ(archive.open(std::move(src)), zim::Error::None);

  int articles = 0;
  for (uint32_t i = 0; i < archive.entryCount(); i++) {
    zim::Entry e;
    ASSERT_EQ(archive.entryAt(i, e), zim::Error::None);
    if (!e.isContent() || e.ns != archive.contentNamespace()) continue;
    if (archive.mimeType(e.mime).rfind("text/html", 0) != 0) continue;
    std::string html;
    ASSERT_EQ(archive.read(e, html), zim::Error::None);
    for (bool reader : {false, true}) {
      const Layout layout = layOut(html, e.title, reader);
      ASSERT_TRUE(layout.ok) << e.path;
      EXPECT_EQ(layout.roundTripWords, layout.words.size()) << e.path;
      // Every collected heading got an anchor the reader can jump to.
      for (size_t h = 0; h < layout.headings.size(); h++) {
        bool found = false;
        for (const auto& a : layout.anchors) found |= a.first == zim::headingAnchor(h);
        EXPECT_TRUE(found) << e.path << " heading " << h;
      }
    }
    articles++;
  }
  EXPECT_GT(articles, 5);
}

TEST_P(ArticleLayout, ClimateChangeReadsInOrder) {
  zim::Archive archive;
  ASSERT_EQ(archive.open(zim::PosixSource::open(GetParam())), zim::Error::None);
  zim::Entry e;
  ASSERT_EQ(archive.findByTitle(archive.contentNamespace(), "Climate change", e), zim::Error::None);
  ASSERT_EQ(archive.resolve(e), zim::Error::None);
  std::string html;
  ASSERT_EQ(archive.read(e, html), zim::Error::None);
  const Layout layout = layOut(html, e.title);
  ASSERT_TRUE(layout.ok);
  EXPECT_GE(layout.pages, 2u);
  const std::string text = joined(layout.words, 40);
  // Heading first, then the lead sentence; no scripts or CSS leaked in.
  EXPECT_EQ(text.rfind("Climate change In common usage, climate change describes global warming", 0), 0u) << text;
  for (const auto& w : layout.words) {
    EXPECT_EQ(w.find(".js"), std::string::npos) << w;
    EXPECT_EQ(w.find('{'), std::string::npos) << w;
  }
}

TEST_P(ArticleLayout, LinksBecomeTapTargets) {
  zim::Archive archive;
  ASSERT_EQ(archive.open(zim::PosixSource::open(GetParam())), zim::Error::None);
  zim::Entry e;
  ASSERT_EQ(archive.findByTitle(archive.contentNamespace(), "Climate change", e), zim::Error::None);
  ASSERT_EQ(archive.resolve(e), zim::Error::None);
  std::string html;
  ASSERT_EQ(archive.read(e, html), zim::Error::None);
  EXPECT_EQ(layOut(html, e.title, false).links, 0u);
  EXPECT_GT(layOut(html, e.title, true).links, 20u);
}

TEST(ArticleLayoutSynthetic, HeadingsLandOnTheirPages) {
  std::string html = "<h1>T</h1>";
  for (int s = 0; s < 6; s++) {
    html += "<h2><span class=\"mw-headline\" id=\"S" + std::to_string(s) + "\">Section " + std::to_string(s) +
            "</span></h2>";
    for (int p = 0; p < 40; p++) html += "<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do.</p>";
  }
  const Layout layout = layOut(html, "T", true);
  ASSERT_TRUE(layout.ok);
  ASSERT_EQ(layout.headings.size(), 7u);
  EXPECT_EQ(layout.headings[3].aliases.at(0), "S2");
  uint16_t previous = 0;
  for (size_t h = 0; h < layout.headings.size(); h++) {
    uint16_t page = UINT16_MAX;
    for (const auto& a : layout.anchors)
      if (a.first == zim::headingAnchor(h)) page = a.second;
    ASSERT_NE(page, UINT16_MAX) << h;
    EXPECT_GE(page, previous) << "headings in page order";
    previous = page;
  }
  EXPECT_GE(previous, 5) << "later sections land on later pages";
}

INSTANTIATE_TEST_SUITE_P(Wikipedia, ArticleLayout, ::testing::Values(kNewNs, kOldNs));

}  // namespace

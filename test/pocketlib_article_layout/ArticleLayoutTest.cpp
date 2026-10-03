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
  std::vector<std::string> words;  // every word on every page, in order
  size_t pages = 0;
  size_t roundTripWords = 0;
};

std::string tmpPath(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

Layout layOut(const std::string& html, const std::string& title) {
  Layout result;
  const std::string xhtml = tmpPath("pocketlib-article.xhtml");
  const std::string pagesPath = tmpPath("pocketlib-article.pages");
  {
    HalFile out;
    EXPECT_TRUE(out.open(xhtml.c_str(), "wb"));
    FileSink sink(out);
    zim::HtmlCleanOptions options;
    options.title = title;
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
    const Layout layout = layOut(html, e.title);
    ASSERT_TRUE(layout.ok) << e.path;
    EXPECT_EQ(layout.roundTripWords, layout.words.size()) << e.path;
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

INSTANTIATE_TEST_SUITE_P(Wikipedia, ArticleLayout, ::testing::Values(kNewNs, kOldNs));

}  // namespace

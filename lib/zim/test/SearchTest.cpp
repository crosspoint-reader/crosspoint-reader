// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Search as you type over openZIM's real Wikipedia sample, with and without
// the sidecar title index.

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include "PosixSource.h"
#include "TitleIndexWriter.h"
#include "ZimArchive.h"
#include "ZimFold.h"
#include "ZimSearch.h"
#include "ZimTitleIndex.h"

namespace {

const std::string kData = ZIM_TEST_DATA_DIR;

class Search : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    ASSERT_EQ(archive.open(
                  zim::openArchiveSource(kData + "/" + GetParam() + "/wikipedia_en_climate_change_mini_2024-06.zim")),
              zim::Error::None);
    zim::TitleIndexWriter w;
    ASSERT_EQ(zim::collectTitles(archive, w, nullptr), zim::Error::None);
    path = (std::filesystem::temp_directory_path() / (std::to_string(getpid()) + "-search-" + GetParam() + ".pltitles"))
               .string();  // per process: ctest runs cases in parallel
    std::string why;
    ASSERT_TRUE(w.write(path, archive.header().uuid, archive.entryCount(), &why)) << why;
    auto src = zim::PosixSource::open(path);
    ASSERT_TRUE(src);
    ASSERT_EQ(index.open(std::move(src)), zim::Error::None);
    cursor = std::make_unique<zim::TitleIndex::Cursor>();
  }

  std::vector<zim::SearchHit> find(std::string_view q, size_t max = 8, bool useIndex = true) {
    std::vector<zim::SearchHit> hits;
    EXPECT_EQ(zim::searchTitles(archive, useIndex ? &index : nullptr, cursor.get(), q, max, hits), zim::Error::None);
    return hits;
  }

  zim::Archive archive;
  zim::TitleIndex index;
  std::unique_ptr<zim::TitleIndex::Cursor> cursor;
  std::string path;
};

INSTANTIATE_TEST_SUITE_P(Wikipedia, Search, ::testing::Values("withns", "nons", "noTitleListingV0"));

TEST_P(Search, ExactMatchComesFirstWhateverTheCase) {
  for (const char* q : {"Climate change", "climate change", "CLIMATE  CHANGE", "climaté change"}) {
    const auto hits = find(q);
    ASSERT_FALSE(hits.empty()) << q;
    EXPECT_TRUE(hits[0].exact) << q << ": " << hits[0].title;
  }
}

TEST_P(Search, HitsStartWithTheQueryAndPointAtDistinctArticles) {
  const std::string key = zim::foldKey("clim");
  const auto hits = find("clim", 20);
  EXPECT_GT(hits.size(), 3u);
  std::set<uint32_t> targets;
  for (size_t i = 0; i < hits.size(); i++) {
    const auto& h = hits[i];
    zim::Entry matched;
    ASSERT_EQ(archive.entryAt(h.entry, matched), zim::Error::None);
    // The title starts with the query, or one of its later words does.
    const std::string title = zim::foldKey(matched.title);
    EXPECT_TRUE(zim::keyHasPrefix(title, key) || title.find(" " + key) != std::string::npos) << h.title;
    if (i > 0 && !hits[i - 1].exact) {
      // Ranked: exact match first, then popularity, never rising.
      EXPECT_FALSE(h.exact) << h.title;
      EXPECT_LE(h.score, hits[i - 1].score) << h.title << " after " << hits[i - 1].title;
    }
    zim::Entry e;
    ASSERT_EQ(archive.entryAt(h.entry, e), zim::Error::None);
    ASSERT_EQ(archive.resolve(e), zim::Error::None);
    EXPECT_TRUE(targets.insert(e.index).second) << h.title << " repeats an article already listed";
  }
}

TEST_P(Search, PopularArticlesComeFirst) {
  // Scores come from redirect counts: the sample has redirects, so some are
  // non-zero, and the first hit has the highest score of those listed.
  for (const char* q : {"c", "clim", "glo"}) {
    const auto hits = find(q, 8);
    ASSERT_FALSE(hits.empty()) << q;
    EXPECT_GT(hits[0].score, 0u) << q << ": " << hits[0].title;
    for (const auto& h : hits) EXPECT_LE(h.score, hits[0].score) << q << ": " << h.title;
  }
}

TEST_P(Search, RedirectsShowTheirArticle) {
  // Any redirect hit is shown as "Article (redirect title)".
  for (const char* q : {"c", "g", "s", "t"}) {
    for (const auto& h : find(q, 8)) {
      zim::Entry e;
      ASSERT_EQ(archive.entryAt(h.entry, e), zim::Error::None);
      if (!e.isRedirect()) {
        EXPECT_EQ(h.title, e.title);
        continue;
      }
      zim::Entry t = e;
      ASSERT_EQ(archive.resolve(t), zim::Error::None);
      if (t.title != e.title) {
        EXPECT_EQ(h.title, t.title + " (" + e.title + ")");
      }
    }
  }
}

TEST(SearchScore, LogScale) {
  EXPECT_EQ(zim::popularityScore(0), 0);
  EXPECT_EQ(zim::popularityScore(1), 0);
  EXPECT_EQ(zim::popularityScore(2), 32);
  EXPECT_EQ(zim::popularityScore(4), 64);
  EXPECT_EQ(zim::popularityScore(16), 128);
  EXPECT_EQ(zim::popularityScore(256), 255);
  EXPECT_EQ(zim::popularityScore(100000), 255);
}

TEST_P(Search, LimitsAndEmptyQueries) {
  EXPECT_TRUE(find("").empty());
  EXPECT_TRUE(find("   ").empty());
  EXPECT_TRUE(find("zzzzqqqq").empty());
  EXPECT_LE(find("c", 3).size(), 3u);
  EXPECT_TRUE(find("c", 0).empty());
}

TEST_P(Search, WithoutAnIndexMatchesCaseSensitively) {
  if (!archive.hasTitleIndex()) GTEST_SKIP() << "no title list in this file";
  const auto hits = find("Climate ch", 8, /*useIndex=*/false);
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(hits[0].title.rfind("Climate ch", 0), 0u);
  EXPECT_TRUE(find("climate ch", 8, false).empty()) << "byte-ordered list: lower case misses";
}

}  // namespace

// Two collections: the same sample in both namespace schemes, one with the
// card's index and one without (as an un-indexed collection on the card).
TEST(SearchMany, ExactFromEverySourceFirstThenTakingTurns) {
  zim::Archive a, b;
  ASSERT_EQ(a.open(zim::openArchiveSource(kData + "/nons/wikipedia_en_climate_change_mini_2024-06.zim")),
            zim::Error::None);
  ASSERT_EQ(b.open(zim::openArchiveSource(kData + "/withns/wikipedia_en_climate_change_mini_2024-06.zim")),
            zim::Error::None);
  zim::TitleIndexWriter w;
  ASSERT_EQ(zim::collectTitles(a, w, nullptr), zim::Error::None);
  const std::string path =
      (std::filesystem::temp_directory_path() / (std::to_string(getpid()) + "-searchmany.pltitles")).string();
  std::string why;
  ASSERT_TRUE(w.write(path, a.header().uuid, a.entryCount(), &why)) << why;
  zim::TitleIndex index;
  ASSERT_EQ(index.open(zim::PosixSource::open(path)), zim::Error::None);
  zim::TitleIndex::Cursor cursor;

  const std::vector<zim::SearchSource> sources = {{&a, &index, &cursor}, {&b, nullptr, nullptr}};
  std::vector<zim::MultiHit> hits;
  ASSERT_EQ(zim::searchMany(sources, "Climate change", 6, hits), zim::Error::None);
  ASSERT_GE(hits.size(), 4u);
  EXPECT_LE(hits.size(), 6u);
  EXPECT_TRUE(hits[0].hit.exact);
  EXPECT_EQ(hits[0].source, 0u);
  EXPECT_TRUE(hits[1].hit.exact);
  EXPECT_EQ(hits[1].source, 1u);
  // After the exact matches the sources alternate while both have results.
  EXPECT_NE(hits[2].source, hits[3].source);

  // Each source's own results match a search of that source alone.
  std::vector<zim::SearchHit> alone;
  ASSERT_EQ(zim::searchTitles(a, &index, &cursor, "Climate change", 6, alone), zim::Error::None);
  size_t k = 0;
  for (const auto& h : hits) {
    if (h.source != 0) continue;
    ASSERT_LT(k, alone.size());
    EXPECT_EQ(h.hit.entry, alone[k++].entry);
  }

  // A failing source is left out; nothing to search is not an error.
  zim::Archive closed;
  const std::vector<zim::SearchSource> withBroken = {{&closed, nullptr, nullptr}, {&a, &index, &cursor}};
  ASSERT_EQ(zim::searchMany(withBroken, "Climate", 4, hits), zim::Error::None);
  EXPECT_FALSE(hits.empty());
  for (const auto& h : hits) EXPECT_EQ(h.source, 1u);
  ASSERT_EQ(zim::searchMany({}, "Climate", 4, hits), zim::Error::None);
  EXPECT_TRUE(hits.empty());
  std::filesystem::remove(path);
}

TEST_P(Search, WordsInsideTitles) {
  // "change" is the second word of "Climate change".
  bool found = false;
  for (const auto& h : find("change", 40)) {
    zim::Entry e;
    ASSERT_EQ(archive.entryAt(h.entry, e), zim::Error::None);
    ASSERT_EQ(archive.resolve(e), zim::Error::None);
    found |= e.title == "Climate change";
    EXPECT_FALSE(h.exact && zim::foldKey(e.title) != "change") << "a word match is never exact: " << h.title;
  }
  if (GetParam() != "noTitleListingV0") EXPECT_TRUE(found);
}

TEST_P(Search, ToleratesTypos) {
  for (const char* q : {"climte change", "clmiate change", "climate chnage", "climate changee"}) {
    const auto hits = find(q, 8);
    bool found = false;
    for (const auto& h : hits) {
      zim::Entry e;
      ASSERT_EQ(archive.entryAt(h.entry, e), zim::Error::None);
      ASSERT_EQ(archive.resolve(e), zim::Error::None);
      found |= e.title == "Climate change";
    }
    EXPECT_TRUE(found) << q;
  }
  EXPECT_TRUE(find("zzzzqqqq", 8).empty()) << "nothing close: nothing found";
}

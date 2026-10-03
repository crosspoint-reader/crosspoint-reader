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
    path = (std::filesystem::temp_directory_path() / ("search-" + GetParam() + ".pltitles")).string();
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
    EXPECT_EQ(zim::foldKey(hits[0].title), "climate change") << q;
  }
}

TEST_P(Search, HitsStartWithTheQueryAndPointAtDistinctArticles) {
  const std::string key = zim::foldKey("clim");
  const auto hits = find("clim", 20);
  EXPECT_GT(hits.size(), 3u);
  std::set<uint32_t> targets;
  std::string previous;
  for (const auto& h : hits) {
    const std::string k = zim::foldKey(h.title);
    EXPECT_TRUE(zim::keyHasPrefix(k, key)) << h.title;
    EXPECT_LE(previous, k) << "title order";
    previous = k;
    zim::Entry e;
    ASSERT_EQ(archive.entryAt(h.entry, e), zim::Error::None);
    ASSERT_EQ(archive.resolve(e), zim::Error::None);
    EXPECT_TRUE(targets.insert(e.index).second) << h.title << " repeats an article already listed";
  }
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

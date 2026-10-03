// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Tests against openZIM's zim-testing-suite: real Kiwix files in both
// namespace schemes ("withns" = old A/I/-, "nons" = new C/M/W/X), a split
// copy, xz and zstd clusters, and ~25 deliberately corrupted files.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "PosixSource.h"
#include "ZimArchive.h"

namespace {

const std::string kData = ZIM_TEST_DATA_DIR;

std::string dataPath(const std::string& scheme, const std::string& name) { return kData + "/" + scheme + "/" + name; }

zim::Error openFile(zim::Archive& a, const std::string& path, const zim::Options& o = zim::Options()) {
  auto src = zim::openArchiveSource(path);
  if (!src) return zim::Error::Io;
  return a.open(std::move(src), o);
}

int compareKey(const zim::Entry& a, const zim::Entry& b, bool byTitle) {
  if (a.ns != b.ns) return static_cast<unsigned char>(a.ns) < static_cast<unsigned char>(b.ns) ? -1 : 1;
  return byTitle ? a.title.compare(b.title) : a.path.compare(b.path);
}

// Counts live allocations so tests can check the archive frees what it takes.
std::atomic<long> gLive{0};
void* countingAllocate(size_t n) {
  ++gLive;
  return std::malloc(n);
}
void countingRelease(void* p) {
  --gLive;
  std::free(p);
}
const zim::Allocator kCounting{&countingAllocate, &countingRelease};

class BothSchemes : public ::testing::TestWithParam<std::string> {};

}  // namespace

INSTANTIATE_TEST_SUITE_P(Zim, BothSchemes, ::testing::Values("withns", "nons"));

TEST_P(BothSchemes, SmallFileOpensWithSaneHeader) {
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath(GetParam(), "small.zim")), zim::Error::None);
  EXPECT_GT(a.entryCount(), 0u);
  EXPECT_GT(a.header().clusterCount, 0u);
  EXPECT_GT(a.mimeTypeCount(), 0u);
  EXPECT_EQ(a.usesNewNamespaces(), GetParam() == "nons");
  EXPECT_EQ(a.contentNamespace(), GetParam() == "nons" ? 'C' : 'A');
  EXPECT_TRUE(a.hasTitleIndex());
}

TEST_P(BothSchemes, MainPageResolvesToHtml) {
  for (const char* file : {"small.zim", "wikipedia_en_climate_change_mini_2024-06.zim"}) {
    zim::Archive a;
    ASSERT_EQ(openFile(a, dataPath(GetParam(), file)), zim::Error::None) << file;
    zim::Entry main;
    ASSERT_EQ(a.mainEntry(main), zim::Error::None) << file;
    EXPECT_TRUE(main.isContent());
    EXPECT_EQ(a.mimeType(main.mime), "text/html") << file;
    std::string html;
    ASSERT_EQ(a.read(main, html), zim::Error::None);
    EXPECT_NE(html.find("<"), std::string::npos);
  }
}

// Walks every entry: the directory must be sorted, every path must be found
// again by binary search, every redirect must resolve, and every content
// entry must read. Covers zstd (2024 files) and xz (2017 Wikibooks).
TEST_P(BothSchemes, EveryEntryReadsAndRoundTrips) {
  for (const char* file :
       {"small.zim", "wikibooks_be_all_nopic_2017-02.zim", "wikipedia_en_climate_change_mini_2024-06.zim"}) {
    SCOPED_TRACE(file);
    zim::Archive a;
    ASSERT_EQ(openFile(a, dataPath(GetParam(), file)), zim::Error::None);
    zim::Entry prev;
    zim::Entry e;
    std::string body;
    uint32_t redirects = 0;
    uint32_t contents = 0;
    for (uint32_t i = 0; i < a.entryCount(); ++i) {
      ASSERT_EQ(a.entryAt(i, e), zim::Error::None) << i;
      if (i > 0) {
        ASSERT_LT(compareKey(prev, e, false), 0) << "directory not sorted at " << i;
      }
      // Path lookup on every entry of the small files, a sample of the big one.
      if (a.entryCount() < 1000 || i % 97 == 0) {
        zim::Entry found;
        ASSERT_EQ(a.findByPath(e.ns, e.path, found), zim::Error::None) << e.path;
        EXPECT_EQ(found.index, i);
      }
      zim::Entry target = e;
      ASSERT_EQ(a.resolve(target), zim::Error::None) << e.path;
      if (e.isRedirect()) ++redirects;
      if (target.isContent() && (a.entryCount() < 1000 || i % 7 == 0)) {
        ASSERT_EQ(a.read(target, body), zim::Error::None) << target.path;
        ++contents;
      }
      prev = e;
    }
    EXPECT_GT(contents, 0u);
    if (std::string(file).find("climate") != std::string::npos) {
      EXPECT_GT(redirects, 1000u);
    }
  }
}

TEST_P(BothSchemes, TitleIndexIsSortedAndSearchable) {
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath(GetParam(), "wikipedia_en_climate_change_mini_2024-06.zim")), zim::Error::None);
  ASSERT_TRUE(a.hasTitleIndex());
  zim::Entry prev;
  zim::Entry e;
  for (uint32_t pos = 0; pos < a.titleCount(); ++pos) {
    ASSERT_EQ(a.titleEntryAt(pos, e), zim::Error::None) << pos;
    if (pos > 0) {
      ASSERT_LE(compareKey(prev, e, true), 0) << "title index not sorted at " << pos;
    }
    if (pos % 211 == 0 && e.ns == a.contentNamespace()) {
      uint32_t lb = 0;
      ASSERT_EQ(a.lowerBoundTitle(e.ns, e.title, lb), zim::Error::None);
      zim::Entry first;
      ASSERT_EQ(a.titleEntryAt(lb, first), zim::Error::None);
      EXPECT_EQ(first.title, e.title);
      zim::Entry found;
      EXPECT_EQ(a.findByTitle(e.ns, e.title, found), zim::Error::None) << e.title;
    }
    prev = e;
  }
}

TEST_P(BothSchemes, FindsClimateChangeAndFollowsARedirect) {
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath(GetParam(), "wikipedia_en_climate_change_mini_2024-06.zim")), zim::Error::None);
  zim::Entry e;
  ASSERT_EQ(a.findByTitle(a.contentNamespace(), "Climate change", e), zim::Error::None);
  ASSERT_EQ(a.resolve(e), zim::Error::None);
  std::string html;
  ASSERT_EQ(a.read(e, html), zim::Error::None);
  EXPECT_GT(html.size(), 10000u);
  EXPECT_NE(html.find("greenhouse"), std::string::npos);

  // Prefix search lands on the first title >= the prefix.
  uint32_t pos = 0;
  ASSERT_EQ(a.lowerBoundTitle(a.contentNamespace(), "Climate", pos), zim::Error::None);
  ASSERT_EQ(a.titleEntryAt(pos, e), zim::Error::None);
  EXPECT_EQ(e.title.rfind("Climate", 0), 0u) << e.title;

  // Some redirect in the title index resolves to readable content.
  bool sawRedirect = false;
  for (uint32_t p = 0; p < a.titleCount() && !sawRedirect; ++p) {
    ASSERT_EQ(a.titleEntryAt(p, e), zim::Error::None);
    if (!e.isRedirect() || e.ns != a.contentNamespace()) continue;
    zim::Entry target = e;
    ASSERT_EQ(a.resolve(target), zim::Error::None);
    EXPECT_TRUE(target.isContent());
    ASSERT_EQ(a.read(target, html), zim::Error::None);
    sawRedirect = true;
  }
  EXPECT_TRUE(sawRedirect);
}

TEST(Zim, NewSchemeHasFrontArticleList) {
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath("nons", "wikipedia_en_climate_change_mini_2024-06.zim")), zim::Error::None);
  ASSERT_TRUE(a.hasArticleList());
  EXPECT_GT(a.articleListCount(), 1000u);
  EXPECT_LT(a.articleListCount(), a.titleCount());
  zim::Entry e;
  for (uint32_t p = 0; p < a.articleListCount(); p += 50) {
    ASSERT_EQ(a.articleListEntryAt(p, e), zim::Error::None);
    EXPECT_EQ(e.ns, 'C');
    EXPECT_FALSE(e.isRedirect());
  }
}

// Recent Kiwix files (e.g. wikipedia_en_all_nopic_2026-06) have no full title
// listing; lookups and prefix search must fall back to the front-article list.
TEST(Zim, FallsBackToFrontArticleListWithoutV0) {
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath("noTitleListingV0", "wikipedia_en_climate_change_mini_2024-06.zim")),
            zim::Error::None);
  ASSERT_TRUE(a.hasArticleList());
  EXPECT_EQ(a.titleSource(), zim::TitleSource::Articles);
  EXPECT_EQ(a.titleCount(), a.articleListCount());

  zim::Entry e;
  ASSERT_EQ(a.findByTitle('C', "Climate change", e), zim::Error::None);
  std::string html;
  ASSERT_EQ(a.read(e, html), zim::Error::None);
  EXPECT_NE(html.find("greenhouse"), std::string::npos);

  uint32_t pos = 0;
  ASSERT_EQ(a.lowerBoundTitle('C', "Clim", pos), zim::Error::None);
  ASSERT_EQ(a.titleEntryAt(pos, e), zim::Error::None);
  EXPECT_EQ(e.title.rfind("Clim", 0), 0u) << e.title;

  zim::Entry prev;
  for (uint32_t p = 0; p < a.titleCount(); ++p) {
    ASSERT_EQ(a.titleEntryAt(p, e), zim::Error::None) << p;
    if (p > 0) {
      ASSERT_LE(compareKey(prev, e, true), 0) << "not sorted at " << p;
    }
    prev = e;
  }
}

TEST(Zim, PrefersTheFullListingWhenPresent) {
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath("nons", "wikipedia_en_climate_change_mini_2024-06.zim")), zim::Error::None);
  EXPECT_EQ(a.titleSource(), zim::TitleSource::Listing);
}

TEST(Zim, MetadataReads) {
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath("nons", "wikipedia_en_climate_change_mini_2024-06.zim")), zim::Error::None);
  std::string lang;
  ASSERT_EQ(a.metadata("Language", lang), zim::Error::None);
  EXPECT_EQ(lang, "eng");
  std::string missing;
  EXPECT_EQ(a.metadata("NoSuchKey", missing), zim::Error::NotFound);
}

// The split copy must read byte-for-byte like the single file.
TEST_P(BothSchemes, SplitFileMatchesSingleFile) {
  zim::Archive whole;
  zim::Archive split;
  ASSERT_EQ(openFile(whole, dataPath(GetParam(), "wikibooks_be_all_nopic_2017-02.zim")), zim::Error::None);
  size_t parts = 0;
  auto src = zim::openArchiveSource(dataPath(GetParam(), "wikibooks_be_all_nopic_2017-02_splitted.zimaa"), &parts);
  ASSERT_TRUE(src);
  EXPECT_EQ(parts, 3u);
  ASSERT_EQ(split.open(std::move(src)), zim::Error::None);
  ASSERT_EQ(whole.entryCount(), split.entryCount());

  zim::Entry a;
  zim::Entry b;
  std::string ba;
  std::string bb;
  for (uint32_t i = 0; i < whole.entryCount(); ++i) {
    ASSERT_EQ(whole.entryAt(i, a), zim::Error::None);
    ASSERT_EQ(split.entryAt(i, b), zim::Error::None);
    ASSERT_EQ(a.path, b.path);
    if (!a.isContent()) continue;
    ASSERT_EQ(whole.read(a, ba), zim::Error::None);
    ASSERT_EQ(split.read(b, bb), zim::Error::None);
    ASSERT_EQ(ba, bb) << a.path;
  }
}

TEST(Zim, OpeningBySplitPrefixAlsoWorks) {
  size_t parts = 0;
  auto src = zim::openArchiveSource(dataPath("nons", "wikibooks_be_all_nopic_2017-02_splitted"), &parts);
  EXPECT_TRUE(src);
  EXPECT_EQ(parts, 3u);
}

TEST(Zim, ClusterCacheHitsAndFreesEverything) {
  {
    zim::Options o;
    o.allocator = &kCounting;
    o.clusterCacheSize = 3;
    zim::Archive a;
    ASSERT_EQ(openFile(a, dataPath("nons", "wikipedia_en_climate_change_mini_2024-06.zim"), o), zim::Error::None);
    zim::Entry e;
    ASSERT_EQ(a.findByTitle('C', "Climate change", e), zim::Error::None);
    ASSERT_EQ(a.resolve(e), zim::Error::None);
    std::string html;
    ASSERT_EQ(a.read(e, html), zim::Error::None);
    const auto decompressedOnce = a.cacheStats().decompressions;
    ASSERT_EQ(a.read(e, html), zim::Error::None);
    EXPECT_EQ(a.cacheStats().decompressions, decompressedOnce) << "second read should hit the cache";
    EXPECT_GT(a.cacheStats().hits, 0u);
  }
  EXPECT_EQ(gLive.load(), 0) << "allocator leak";
}

// Allocator with a byte budget, like PSRAM shared with fonts on the device.
std::atomic<size_t> gBudgetLive{0};
std::atomic<size_t> gBudgetPeak{0};
size_t gBudget = SIZE_MAX;
struct BudgetHeader {
  size_t size;
  size_t pad;
};
void* budgetAllocate(size_t n) {
  if (gBudgetLive + n > gBudget) return nullptr;
  auto* h = static_cast<BudgetHeader*>(std::malloc(sizeof(BudgetHeader) + n));
  if (!h) return nullptr;
  h->size = n;
  gBudgetLive += n;
  if (gBudgetLive > gBudgetPeak) gBudgetPeak = gBudgetLive.load();
  return h + 1;
}
void budgetRelease(void* p) {
  if (!p) return;
  auto* h = static_cast<BudgetHeader*>(p) - 1;
  gBudgetLive -= h->size;
  std::free(h);
}
const zim::Allocator kBudget{budgetAllocate, budgetRelease};

// Regression (device, 2026-10-03): the first article opened, every later one
// failed with "out of memory". The cache decoded a new cluster before
// evicting, so a full cache needed room for one cluster more than its size.
TEST(Zim, NewClusterFitsWhereOneClusterFits) {
  const std::string path = dataPath("nons", "wikipedia_en_climate_change_mini_2024-06.zim");
  // Two articles in different compressed clusters; peak memory for each alone.
  std::vector<uint32_t> picks;
  size_t onePeak = 0;
  {
    zim::Archive a;
    zim::Options o;
    o.allocator = &kBudget;
    ASSERT_EQ(openFile(a, path, o), zim::Error::None);
    uint32_t firstCluster = UINT32_MAX;
    std::string body;
    for (uint32_t i = 0; i < a.entryCount() && picks.size() < 2; i++) {
      zim::Entry e;
      ASSERT_EQ(a.entryAt(i, e), zim::Error::None);
      if (!e.isContent() || e.cluster == firstCluster) continue;
      a.clearCache();
      gBudgetPeak = gBudgetLive.load();
      const size_t base = gBudgetLive;
      const auto before = a.cacheStats().decompressions;
      ASSERT_EQ(a.read(e, body), zim::Error::None);
      if (a.cacheStats().decompressions == before) continue;  // uncompressed cluster
      onePeak = std::max(onePeak, gBudgetPeak - base);
      firstCluster = e.cluster;
      picks.push_back(i);
    }
  }
  ASSERT_EQ(picks.size(), 2u);
  ASSERT_EQ(gBudgetLive.load(), 0u);

  for (size_t cacheSize : {1u, 2u, 3u}) {
    gBudget = onePeak;  // room for decoding one cluster, nothing more
    zim::Archive a;
    zim::Options o;
    o.allocator = &kBudget;
    o.clusterCacheSize = cacheSize;
    ASSERT_EQ(openFile(a, path, o), zim::Error::None);
    std::string body;
    for (int round = 0; round < 3; round++) {
      for (uint32_t i : picks) {
        zim::Entry e;
        ASSERT_EQ(a.entryAt(i, e), zim::Error::None);
        EXPECT_EQ(a.read(e, body), zim::Error::None) << "cache " << cacheSize << ", round " << round;
      }
    }
  }
  gBudget = SIZE_MAX;
  EXPECT_EQ(gBudgetLive.load(), 0u);
}

TEST(Zim, CacheOfOneStillWorksAcrossClusters) {
  zim::Options o;
  o.clusterCacheSize = 1;
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath("nons", "wikipedia_en_climate_change_mini_2024-06.zim"), o), zim::Error::None);
  zim::Entry e;
  std::string body;
  uint32_t read = 0;
  for (uint32_t i = 0; i < a.entryCount() && read < 300; i += 37) {
    ASSERT_EQ(a.entryAt(i, e), zim::Error::None);
    if (!e.isContent()) continue;
    ASSERT_EQ(a.read(e, body), zim::Error::None);
    ++read;
  }
  EXPECT_GT(read, 100u);
}

TEST(Zim, RefusesClustersAboveTheLimit) {
  zim::Options o;
  o.maxClusterBytes = 64 * 1024;  // Kiwix clusters are ~2 MiB
  zim::Archive a;
  ASSERT_EQ(openFile(a, dataPath("nons", "wikipedia_en_climate_change_mini_2024-06.zim"), o), zim::Error::None);
  zim::Entry e;
  ASSERT_EQ(a.findByTitle('C', "Climate change", e), zim::Error::None);
  ASSERT_EQ(a.resolve(e), zim::Error::None);
  std::string html;
  EXPECT_EQ(a.read(e, html), zim::Error::TooLarge);
}

TEST(Zim, RejectsNonZimData) {
  zim::Archive a;
  EXPECT_NE(openFile(a, dataPath("nons", "small.zim.embedded")), zim::Error::None);
  EXPECT_EQ(openFile(a, kData + "/does/not/exist.zim"), zim::Error::Io);
}

// Every corrupted file must either fail to open or fail cleanly on the bad
// part; never crash or read out of bounds (CI runs this under ASan/UBSan).
// For each, at least one operation must report an error.
TEST_P(BothSchemes, CorruptFilesFailCleanly) {
  const char* files[] = {
      "invalid.bad_mimetype_in_dirent.zim",
      "invalid.bad_mimetype_list.zim",
      "invalid.invalid_checksumpos.zim",
      "invalid.invalid_mimelistpos.zim",
      "invalid.misaligned_offset_of_first_blob_in_cluster_9.zim",
      "invalid.misaligned_offset_of_first_blob_in_cluster_10.zim",
      "invalid.misaligned_offset_of_first_blob_in_cluster_11.zim",
      "invalid.nonsorted_dirent_table.zim",
      "invalid.nonsorted_title_index.zim",
      "invalid.offset_in_cluster.zim",
      "invalid.outofbounds_clusterptrpos.zim",
      "invalid.outofbounds_first_clusterptr.zim",
      "invalid.outofbounds_first_direntptr.zim",
      "invalid.outofbounds_first_title_entry.zim",
      "invalid.outofbounds_last_direntptr.zim",
      "invalid.outofbounds_last_title_entry.zim",
      "invalid.outofbounds_titleptrpos.zim",
      "invalid.outofbounds_urlptrpos.zim",
      "invalid.smaller_than_header.zim",
      "invalid.too_large_offset_of_first_blob_in_cluster.zim",
      "invalid.too_small_offset_of_first_blob_in_cluster_0.zim",
      "invalid.too_small_offset_of_first_blob_in_cluster_4.zim",
      "invalid.too_small_offset_of_first_blob_in_cluster_7.zim",
  };
  for (const char* file : files) {
    SCOPED_TRACE(file);
    zim::Archive a;
    if (openFile(a, dataPath(GetParam(), file)) != zim::Error::None) continue;  // refused at open: fine

    bool sawError = false;
    zim::Entry prev;
    zim::Entry e;
    std::string body;
    for (uint32_t i = 0; i < a.entryCount(); ++i) {
      if (a.entryAt(i, e) != zim::Error::None) {
        sawError = true;
        continue;
      }
      if (i > 0 && compareKey(prev, e, false) >= 0) sawError = true;  // unsorted directory
      prev = e;
      zim::Entry target = e;
      if (a.resolve(target) != zim::Error::None) {
        sawError = true;
        continue;
      }
      if (target.isContent() && a.read(target, body) != zim::Error::None) sawError = true;
    }
    zim::Entry tprev;
    for (uint32_t p = 0; p < a.titleCount(); ++p) {
      if (a.titleEntryAt(p, e) != zim::Error::None) {
        sawError = true;
        continue;
      }
      if (p > 0 && compareKey(tprev, e, true) > 0) sawError = true;  // unsorted title index
      tprev = e;
    }
    EXPECT_TRUE(sawError) << "corruption went unnoticed";
  }
}

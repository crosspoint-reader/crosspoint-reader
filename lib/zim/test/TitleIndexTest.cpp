// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Search-key folding and the title index: synthetic indexes deep enough to
// have inner levels, indexes built from openZIM's real test files, and
// damaged index files.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "PosixSource.h"
#include "TitleIndexWriter.h"
#include "ZimArchive.h"
#include "ZimFold.h"
#include "ZimTitleIndex.h"

namespace {

const std::string kData = ZIM_TEST_DATA_DIR;
const uint8_t kUuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

std::string tempPath(const std::string& name) { return ::testing::TempDir() + "pl_" + name; }

zim::Error openIndex(zim::TitleIndex& index, const std::string& path) {
  auto src = zim::PosixSource::open(path);
  if (!src) return zim::Error::Io;
  return index.open(std::move(src));
}

// All records from `key` onward, up to `limit`.
std::vector<zim::TitleRecord> scanFrom(zim::TitleIndex& index, const std::string& key, size_t limit) {
  auto cursor = std::make_unique<zim::TitleIndex::Cursor>();
  std::vector<zim::TitleRecord> out;
  EXPECT_EQ(index.seek(key, *cursor), zim::Error::None);
  zim::TitleRecord r;
  while (out.size() < limit && index.next(*cursor, r) == zim::Error::None) out.push_back(r);
  return out;
}

}  // namespace

TEST(Fold, CaseAccentsSpacesAndPunctuation) {
  EXPECT_EQ(zim::foldKey("Forbidden City"), "forbidden city");
  EXPECT_EQ(zim::foldKey("  FÖRBIDDEN \t CITY  "), "forbidden city");
  EXPECT_EQ(zim::foldKey("Straße"), "strasse");
  EXPECT_EQ(zim::foldKey("Ångström"), "angstrom");
  EXPECT_EQ(zim::foldKey("Łódź"), "lodz");
  EXPECT_EQ(zim::foldKey("Ærøskøbing"), "aeroskobing");
  EXPECT_EQ(zim::foldKey("Việt Nam"), "viet nam");
  EXPECT_EQ(zim::foldKey("ΕΛΛΆΔΑ"), zim::foldKey("ελλάδα"));
  EXPECT_EQ(zim::foldKey("МОСКВА"), "москва");
  EXPECT_EQ(zim::foldKey("北京市"), "北京市");
  EXPECT_EQ(zim::foldKey("Rock ’n’ roll — history"), "rock 'n' roll - history");
  EXPECT_EQ(zim::foldKey("co­operate"), "cooperate");  // soft hyphen dropped
  EXPECT_EQ(zim::foldKey("ﬁsh"), "fish");              // ligature
  EXPECT_EQ(zim::foldKey(""), "");
}

TEST(Fold, InvalidUtf8PassesThroughAndLongKeysAreCut) {
  EXPECT_EQ(zim::foldKey(std::string("A\xff"
                                     "B")),
            std::string("a\xff"
                        "b"));
  std::string longTitle;
  for (int i = 0; i < 300; ++i) longTitle += "é";  // folds to 300 one-byte "e"
  EXPECT_EQ(zim::foldKey(longTitle), std::string(zim::kMaxKeyBytes, 'e'));
  std::string greek;
  for (int i = 0; i < 200; ++i) greek += "λ";  // 2 bytes each, not folded further
  const std::string g = zim::foldKey(greek);
  EXPECT_LE(g.size(), zim::kMaxKeyBytes);
  EXPECT_EQ(g.size() % 2, 0u) << "cut inside a character";
}

TEST(TitleIndex, SyntheticIndexHasLevelsAndFindsEveryPrefix) {
  std::mt19937 rng(7);
  const char letters[] = "abcdefghij éxyz";
  std::vector<std::pair<std::string, uint32_t>> items;
  zim::TitleIndexWriter w;
  for (uint32_t i = 0; i < 120000; ++i) {
    std::string t;
    const int len = 1 + static_cast<int>(rng() % 24);
    for (int k = 0; k < len; ++k) t += letters[rng() % (sizeof letters - 1)];
    if (i % 1000 == 0) t = "duplicate";  // equal keys spanning leaves
    const std::string key = zim::foldKey(t);
    items.emplace_back(key, i);
    w.add(key, i);
  }
  const std::string path = tempPath("synthetic.pltitles");
  std::string why;
  ASSERT_TRUE(w.write(path, kUuid, 120000, &why)) << why;
  std::sort(items.begin(), items.end());

  zim::TitleIndex index;
  ASSERT_EQ(openIndex(index, path), zim::Error::None);
  EXPECT_EQ(index.recordCount(), items.size());
  EXPECT_GE(index.header().levels, 1u);
  EXPECT_GT(index.header().leafCount, 100u);

  // A full scan returns exactly the sorted records.
  const auto all = scanFrom(index, "", items.size() + 1);
  ASSERT_EQ(all.size(), items.size());
  for (size_t i = 0; i < all.size(); ++i) {
    ASSERT_EQ(all[i].key, items[i].first) << i;
    ASSERT_EQ(all[i].entry, items[i].second) << i;
  }

  // seek() lands on the first record >= the probe, for keys present and absent.
  for (int trial = 0; trial < 3000; ++trial) {
    std::string probe;
    if (trial % 2 == 0) {
      probe = items[rng() % items.size()].first.substr(0, 1 + rng() % 4);
    } else {
      const int len = 1 + static_cast<int>(rng() % 6);
      for (int k = 0; k < len; ++k) probe += letters[rng() % (sizeof letters - 1)];
      probe = zim::foldKey(probe);
    }
    const auto it = std::lower_bound(items.begin(), items.end(), std::make_pair(probe, 0u));
    const auto got = scanFrom(index, probe, 1);
    if (it == items.end()) {
      EXPECT_TRUE(got.empty()) << probe;
    } else {
      ASSERT_EQ(got.size(), 1u) << probe;
      EXPECT_EQ(got[0].key, it->first) << probe;
      EXPECT_EQ(got[0].entry, it->second) << probe;
    }
  }
  const auto dup = scanFrom(index, "duplicate", 200);
  size_t dups = 0;
  for (const auto& r : dup) dups += r.key == "duplicate";
  EXPECT_EQ(dups, 120u);
  std::remove(path.c_str());
}

TEST(TitleIndex, EmptyAndSingleLeafIndexes) {
  for (int n : {0, 1, 5}) {
    zim::TitleIndexWriter w;
    for (int i = 0; i < n; ++i) w.add(std::string(1, static_cast<char>('a' + i)), static_cast<uint32_t>(i));
    const std::string path = tempPath("small" + std::to_string(n) + ".pltitles");
    std::string why;
    ASSERT_TRUE(w.write(path, kUuid, 10, &why)) << why;
    zim::TitleIndex index;
    ASSERT_EQ(openIndex(index, path), zim::Error::None);
    EXPECT_EQ(index.header().levels, 0u);
    EXPECT_EQ(scanFrom(index, "", 100).size(), static_cast<size_t>(n));
    EXPECT_TRUE(scanFrom(index, "zzz", 100).empty());
    std::remove(path.c_str());
  }
}

TEST(TitleIndex, RejectsDamagedFiles) {
  zim::TitleIndexWriter w;
  for (uint32_t i = 0; i < 5000; ++i) w.add("title " + std::to_string(i), i);
  const std::string path = tempPath("damaged.pltitles");
  std::string why;
  ASSERT_TRUE(w.write(path, kUuid, 5000, &why)) << why;
  std::string bytes;
  {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.append(buf, n);
    std::fclose(f);
  }
  const auto writeVariant = [&](const std::string& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  };
  zim::TitleIndex index;

  std::string bad = bytes;
  bad[0] = 'X';
  writeVariant(bad);
  EXPECT_EQ(openIndex(index, path), zim::Error::BadMagic);

  bad = bytes;
  bad[16] = 99;  // fold version
  writeVariant(bad);
  EXPECT_EQ(openIndex(index, path), zim::Error::BadVersion);

  writeVariant(bytes.substr(0, bytes.size() - 100));  // not whole pages
  EXPECT_EQ(openIndex(index, path), zim::Error::BadHeader);

  // Scribbled pages must fail cleanly (no crash, no endless loop) whatever
  // the outcome of each search.
  std::mt19937 rng(3);
  for (int round = 0; round < 40; ++round) {
    bad = bytes;
    for (int k = 0; k < 64; ++k) bad[4096 + rng() % (bad.size() - 4096)] = static_cast<char>(rng());
    writeVariant(bad);
    if (openIndex(index, path) != zim::Error::None) continue;
    auto cursor = std::make_unique<zim::TitleIndex::Cursor>();
    if (index.seek("title 2", *cursor) != zim::Error::None) continue;
    zim::TitleRecord r;
    for (int i = 0; i < 10000 && index.next(*cursor, r) == zim::Error::None; ++i) {
    }
  }
  std::remove(path.c_str());
}

class RealFiles : public ::testing::TestWithParam<std::string> {};
INSTANTIATE_TEST_SUITE_P(Zim, RealFiles, ::testing::Values("withns", "nons", "noTitleListingV0"));

TEST_P(RealFiles, IndexFindsTitlesWhateverTheCase) {
  zim::Archive a;
  ASSERT_EQ(a.open(zim::openArchiveSource(kData + "/" + GetParam() + "/wikipedia_en_climate_change_mini_2024-06.zim")),
            zim::Error::None);
  zim::TitleIndexWriter w;
  ASSERT_EQ(zim::collectTitles(a, w, nullptr), zim::Error::None);
  EXPECT_GT(w.size(), 1000u);
  const std::string path = tempPath(GetParam() + ".pltitles");
  std::string why;
  ASSERT_TRUE(w.write(path, a.header().uuid, a.entryCount(), &why)) << why;

  zim::TitleIndex index;
  ASSERT_EQ(openIndex(index, path), zim::Error::None);
  EXPECT_TRUE(index.matches(a));

  // Lower case, odd spacing and accents still find "Climate change".
  bool found = false;
  for (const auto& r : scanFrom(index, zim::foldKey("  CLIMATE   chänge"), 50)) {
    if (!zim::keyHasPrefix(r.key, "climate change")) break;
    zim::Entry e;
    ASSERT_EQ(a.entryAt(r.entry, e), zim::Error::None);
    if (e.title == "Climate change") found = true;
  }
  EXPECT_TRUE(found);

  // Every indexed record points at an entry whose folded title is its key.
  const auto all = scanFrom(index, "", w.size() + 1);
  ASSERT_EQ(all.size(), w.size());
  for (size_t i = 0; i < all.size(); i += 37) {
    zim::Entry e;
    ASSERT_EQ(a.entryAt(all[i].entry, e), zim::Error::None);
    EXPECT_EQ(zim::foldKey(e.title), all[i].key);
    EXPECT_EQ(e.ns, a.contentNamespace());
  }

  // An index built for one file is refused for another.
  zim::Archive other;
  ASSERT_EQ(other.open(zim::openArchiveSource(kData + "/" + GetParam() + "/small.zim")), zim::Error::None);
  EXPECT_FALSE(index.matches(other));
  std::remove(path.c_str());
}

// Unit tests for the minimal Dictionary engine, run against the en-es fixture.
// Dictionary.cpp reads the index files via the HalStorage host stub.
//
// findSimilar(word, maxResults, cachePath) resolves the dictionary location by
// reading "<cachePath>/dictionary.bin", whose contents are the dictionary file
// stem (folder + base name, no extension). Each test writes such a dictionary.bin
// into a temp cache dir pointing at the fixture stem.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "util/Dictionary.h"

namespace {

const std::string kFixtureStem = std::string(DICT_FIXTURE_DIR) + "/en-es/en-es";

// Write a dictionary.bin pointing at `stem` into a fresh temp cache dir (named
// after `tag`, under GoogleTest's managed temp directory) and return that dir
// path (suitable as findSimilar's cachePath argument).
std::string cacheDirPointingAt(const std::string& stem, const std::string& tag) {
  const std::string cacheDir = testing::TempDir() + tag;
  std::filesystem::create_directories(cacheDir);
  std::ofstream bin(cacheDir + "/dictionary.bin", std::ios::binary | std::ios::trunc);
  bin << stem;
  return cacheDir;
}

std::string cacheDirPointingAtFixture() { return cacheDirPointingAt(kFixtureStem, "dict_engine_with_dict"); }

// A temp cache dir with no dictionary.bin (forces the global fallback, which is
// absent on the host).
std::string emptyCacheDir() {
  const std::string cacheDir = testing::TempDir() + "dict_engine_empty";
  std::filesystem::create_directories(cacheDir);
  std::error_code ec;
  std::filesystem::remove(cacheDir + "/dictionary.bin", ec);
  return cacheDir;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST(DictFindSimilar, ReturnsExactHeadwordForSingleTypo) {
  const std::string cache = cacheDirPointingAtFixture();
  // "aple" is one insertion away from the headword "apple".
  const auto results = Dictionary::findSimilar("aple", 6, cache.c_str());
  EXPECT_TRUE(contains(results, "apple")) << "expected 'apple' among suggestions";
}

TEST(DictFindSimilar, ReturnsMultipleNearbyHeadwords) {
  const std::string cache = cacheDirPointingAtFixture();
  // "lood" is within edit distance 2 of the _oo_ cluster: food/good (1),
  // book/cook/door (2).
  const auto results = Dictionary::findSimilar("lood", 6, cache.c_str());
  EXPECT_TRUE(contains(results, "food"));
  EXPECT_TRUE(contains(results, "good"));
  EXPECT_TRUE(contains(results, "book"));
  EXPECT_TRUE(contains(results, "cook"));
}

TEST(DictFindSimilar, RespectsMaxResults) {
  const std::string cache = cacheDirPointingAtFixture();
  const auto results = Dictionary::findSimilar("lood", 2, cache.c_str());
  EXPECT_LE(results.size(), 2u);
}

TEST(DictFindSimilar, SortsClosestFirst) {
  const std::string cache = cacheDirPointingAtFixture();
  // food/good are distance 1 from "lood"; book/cook/door are distance 2. The
  // closest match must come first.
  const auto results = Dictionary::findSimilar("lood", 6, cache.c_str());
  ASSERT_FALSE(results.empty());
  EXPECT_TRUE(results.front() == "food" || results.front() == "good")
      << "closest match should sort first, got: " << results.front();
}

TEST(DictFindSimilar, ReturnsEmptyForFarWord) {
  const std::string cache = cacheDirPointingAtFixture();
  const auto results = Dictionary::findSimilar("zzzzzzzz", 6, cache.c_str());
  EXPECT_TRUE(results.empty());
}

TEST(DictFindSimilar, ReturnsEmptyWhenNoDictionary) {
  const std::string cache = emptyCacheDir();
  const auto results = Dictionary::findSimilar("grace", 6, cache.c_str());
  EXPECT_TRUE(results.empty());
}

// getStemVariants strips inflectional/derivational suffixes (and a few prefixes)
// to produce candidate base forms. These tests pin the behavior so the
// allocation-reduction refactor (reusable scratch buffer + inline dedup) stays
// equivalent to the previous substr/concat implementation.

TEST(DictStemVariants, TooShortYieldsNothing) { EXPECT_TRUE(Dictionary::getStemVariants("go").empty()); }

TEST(DictStemVariants, RegularPlural) { EXPECT_TRUE(contains(Dictionary::getStemVariants("cats"), "cat")); }

TEST(DictStemVariants, IesPlural) {
  const auto v = Dictionary::getStemVariants("parties");
  EXPECT_TRUE(contains(v, "party"));
}

TEST(DictStemVariants, PastTense) { EXPECT_TRUE(contains(Dictionary::getStemVariants("walked"), "walk")); }

TEST(DictStemVariants, ProgressiveWithDoubledConsonant) {
  const auto v = Dictionary::getStemVariants("running");
  EXPECT_TRUE(contains(v, "run"));
}

TEST(DictStemVariants, PrefixRemoval) { EXPECT_TRUE(contains(Dictionary::getStemVariants("unhappy"), "happy")); }

TEST(DictStemVariants, NoDuplicates) {
  const auto v = Dictionary::getStemVariants("blesses");
  std::vector<std::string> sorted = v;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end())
      << "getStemVariants must not return duplicate variants";
}

// Unit tests for the Dictionary engine's fuzzy lookup (Dictionary::findSimilar),
// run against the en-es fixture (test/dictionaries/en-es), whose index holds a
// few hundred ordinary English headwords — a realistic corpus for fuzzy matching.
// Dictionary.cpp reads the index / offset-table files via the HalStorage host stub.
//
// findSimilar(word, maxResults, cachePath) resolves the dictionary location by
// reading "<cachePath>/dictionary.bin", whose contents are the dictionary file
// stem (folder + base name, no extension). Each test writes such a dictionary.bin
// into a temp cache dir pointing at the fixture stem.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "util/Dictionary.h"

namespace {

const std::string kFixtureStem = std::string(DICT_FIXTURE_DIR) + "/en-es/en-es";
// 1500 synthetic "fuzzy_word_NNNNN" headwords with a real .idx.fpi (~47 fence
// groups), so lookups for words far past offset 0 exercise the .fpi bracket
// binary search in Dictionary::findSimilar (a 6-page dict like en-es always
// clamps the scan window back to offset 0).
const std::string kFuzzyStem = std::string(DICT_FIXTURE_DIR) + "/fuzzy-fpi/fuzzy-fpi";
// 100k synthetic headwords ("all_prep_word_*"), shipped deliberately without
// .idx.fpi so the device prep pipeline generates them — see
// test/data/dictionary-sources/prep-all.json. Reused here as a large real .idx
// to exhaustively exercise Dictionary::generateFpi / binarySearchFpi at scale.
const std::string kPrepAllStem = std::string(DICT_FIXTURE_DIR) + "/prep-all/prep-all";

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

std::string fixtureCopyStem(const std::string& tag) {
  const std::string dir = testing::TempDir() + tag;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::filesystem::copy(std::filesystem::path(kFixtureStem).parent_path(), dir,
                        std::filesystem::copy_options::recursive);
  return dir + "/en-es";
}
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

// Copies only kPrepAllStem's .idx (not the 23MB .dict.dz / 5MB .syn.dz, which this
// suite never reads) into a fresh temp dir unique to `tag`, and returns the copy's
// stem path. Each caller MUST pass a distinct tag: CTest runs discovered gtest
// cases as separate parallel processes (ctest -j), so two tests sharing one temp
// dir would race on the same .idx/.idx.fpi/.syn files mid-generation.
std::string prepAllIdxCopyStem(const std::string& tag) {
  const std::string dir = testing::TempDir() + "dict_engine_prep_all_fpi_" + tag;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string dstStem = dir + "/prep-all";
  std::filesystem::copy_file(kPrepAllStem + ".idx", dstStem + ".idx");
  return dstStem;
}

struct IdxEntry {
  std::string word;
  uint32_t offset;
  uint32_t size;
};

// Parses a StarDict .idx file (word\0 + 4-byte-BE offset + 4-byte-BE size, sorted)
// into (word, offset, size) triples — the ground truth for round-trip lookup checks.
std::vector<IdxEntry> parseIdxFile(const std::string& path) {
  std::vector<IdxEntry> entries;
  std::ifstream f(path, std::ios::binary);
  if (!f) return entries;
  std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

  size_t pos = 0;
  while (pos < data.size()) {
    size_t null = pos;
    while (null < data.size() && data[null] != '\0') null++;
    if (null >= data.size() || null + 9 > data.size()) break;
    const auto* b = reinterpret_cast<const unsigned char*>(data.data() + null + 1);
    IdxEntry e;
    e.word.assign(data.data() + pos, null - pos);
    e.offset = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
               (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
    e.size = (static_cast<uint32_t>(b[4]) << 24) | (static_cast<uint32_t>(b[5]) << 16) |
             (static_cast<uint32_t>(b[6]) << 8) | static_cast<uint32_t>(b[7]);
    entries.push_back(std::move(e));
    pos = null + 9;
  }
  return entries;
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
// .fpi (Fenced Prefix Index) is the exact-match lookup fast path (Dictionary::locate /
// resolveAltForm no longer consults deprecated formats).

TEST(DictLookupFpi, FindsDefinitionWhenFpiExists) {
  const std::string cache = cacheDirPointingAtFixture();
  EXPECT_FALSE(Dictionary::lookup("apple", {}, cache.c_str()).empty());
}

TEST(DictLookupFpi, FallsBackToLinearScanWhenFpiMissing) {
  const std::string stem = fixtureCopyStem("dict_engine_without_fpi");
  std::filesystem::remove(stem + ".idx.fpi");
  const std::string cache = cacheDirPointingAt(stem, "dict_engine_without_fpi_cache");
  EXPECT_FALSE(Dictionary::lookup("apple", {}, cache.c_str()).empty());
}

TEST(DictLookupFpi, ReturnsEmptyForUnknownWord) {
  const std::string cache = cacheDirPointingAtFixture();
  EXPECT_TRUE(Dictionary::lookup("zzzzzzzz_not_a_word", {}, cache.c_str()).empty());
}

// Front-coding within a fencep group distinguishes "confirmed complete" fence words
// (an embedded null terminates the real word early) from "ambiguous" ones (the
// 6-byte window is fully packed, so the entry is only usable as a one-sided bound).
// A cluster of headwords sharing an 8+ char prefix — spanning several .idx sectors —
// exercises that boundary logic directly, mirroring the intent of the on-device
// prep-cspt-prefix-collision fixture for the (now superseded) .cspt format.
TEST(DictLookupFpi, HandlesPrefixCollisions) {
  const std::string dir = testing::TempDir() + "dict_engine_fpi_prefix_collision";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string stem = dir + "/collide";

  std::vector<std::string> words;
  for (int i = 0; i < 300; i++) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "collision%04d", i);
    words.emplace_back(buf);
  }
  for (int i = 0; i < 300; i++) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "collisionx%04d", i);
    words.emplace_back(buf);
  }
  words.emplace_back("a");
  words.emplace_back("ab");
  words.emplace_back("collision");  // exact prefix of the "collisionNNNN" cluster
  std::sort(words.begin(), words.end());
  words.erase(std::unique(words.begin(), words.end()), words.end());

  {
    std::ofstream idxOut(stem + ".idx", std::ios::binary | std::ios::trunc);
    uint32_t off = 0;
    for (const auto& w : words) {
      const std::string def = "def:" + w;
      idxOut.write(w.data(), static_cast<std::streamsize>(w.size()));
      idxOut.put('\0');
      const uint32_t size = static_cast<uint32_t>(def.size());
      const uint8_t suffix[8] = {static_cast<uint8_t>(off >> 24),  static_cast<uint8_t>(off >> 16),
                                 static_cast<uint8_t>(off >> 8),   static_cast<uint8_t>(off),
                                 static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
                                 static_cast<uint8_t>(size >> 8),  static_cast<uint8_t>(size)};
      idxOut.write(reinterpret_cast<const char*>(suffix), sizeof(suffix));
      off += size;
    }
  }

  ASSERT_TRUE(Dictionary::generateFpi((stem + ".idx").c_str(), (stem + ".idx.fpi").c_str(), 8));

  const std::string cache = cacheDirPointingAt(stem, "dict_engine_fpi_prefix_collision_cache");
  size_t misses = 0;
  std::string firstMiss;
  for (const auto& w : words) {
    if (!Dictionary::locate(w, {}, cache.c_str()).found) {
      if (misses == 0) firstMiss = w;
      misses++;
    }
  }
  EXPECT_EQ(misses, 0u) << "first miss: " << firstMiss << " (" << misses << "/" << words.size() << ")";

  // Words that are near-misses of real entries must not be found.
  EXPECT_FALSE(Dictionary::locate("collision9999", {}, cache.c_str()).found);
  EXPECT_FALSE(Dictionary::locate("collisio", {}, cache.c_str()).found);
}

// Exhaustive round trip: generate .idx.fpi for the 100k-word prep-all fixture's raw
// .idx in a single pass, then look up every headword and verify the exact
// offset/size match the ground truth parsed directly from .idx.
TEST(DictLookupFpi, ExhaustiveRoundTripAgainstPrepAll) {
  const std::string stem = prepAllIdxCopyStem("exhaustive");
  ASSERT_TRUE(Dictionary::generateFpi((stem + ".idx").c_str(), (stem + ".idx.fpi").c_str(), 8))
      << "generateFpi failed for the prep-all fixture";

  const auto entries = parseIdxFile(stem + ".idx");
  ASSERT_GT(entries.size(), 90000u) << "prep-all fixture unexpectedly small; regenerate test fixtures "
                                       "(python3 test/data/generate_dictionaries.py prep-all)";

  const std::string cache = cacheDirPointingAt(stem, "dict_engine_prep_all_fpi_cache");

  size_t misses = 0;
  std::string firstMiss;
  for (const auto& e : entries) {
    const auto loc = Dictionary::locate(e.word, {}, cache.c_str());
    if (!loc.found || loc.offset != e.offset || loc.size != e.size) {
      if (misses == 0) firstMiss = e.word;
      misses++;
    }
  }
  EXPECT_EQ(misses, 0u) << "first miss: " << firstMiss << " (" << misses << "/" << entries.size() << " total)";
}

// Exhaustive ordinal round trip: build a synthetic .syn over the ~100k-entry
// prep-all fixture's raw .idx where synonym "syn_word_NNNNNN" maps to
// originalIdx=N for every 0-based ordinal N, generate .idx.fpi/.syn.fpi, then
// resolve every synonym back through the public resolveAltForm() call path
// (locate -> wordAtOrdinal -> binarySearchFpiOrdinal) and verify it lands on
// the exact headword at ordinal N in .idx. Exercises binarySearchFpiOrdinal's
// per-group cumulative-ordinal field at scale, including group boundaries.
TEST(DictLookupFpi, ExhaustiveOrdinalRoundTripAgainstPrepAll) {
  const std::string stem = prepAllIdxCopyStem("ordinal");
  const auto entries = parseIdxFile(stem + ".idx");
  ASSERT_GT(entries.size(), 90000u) << "prep-all fixture unexpectedly small; regenerate test fixtures "
                                       "(python3 test/data/generate_dictionaries.py prep-all)";

  {
    std::ofstream synOut(stem + ".syn", std::ios::binary | std::ios::trunc);
    for (uint32_t i = 0; i < entries.size(); i++) {
      char word[32];
      std::snprintf(word, sizeof(word), "syn_word_%06u", i);
      synOut.write(word, static_cast<std::streamsize>(std::strlen(word)));
      synOut.put('\0');
      const uint8_t idxBuf[4] = {static_cast<uint8_t>(i >> 24), static_cast<uint8_t>(i >> 16),
                                 static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i)};
      synOut.write(reinterpret_cast<const char*>(idxBuf), sizeof(idxBuf));
    }
  }

  ASSERT_TRUE(Dictionary::generateFpi((stem + ".idx").c_str(), (stem + ".idx.fpi").c_str(), 8))
      << "generateFpi failed for the prep-all fixture's .idx";
  ASSERT_TRUE(Dictionary::generateFpi((stem + ".syn").c_str(), (stem + ".syn.fpi").c_str(), 4))
      << "generateFpi failed for the synthetic .syn";

  const std::string cache = cacheDirPointingAt(stem, "dict_engine_prep_all_ordinal_cache");

  size_t misses = 0;
  std::string firstMiss;
  for (uint32_t i = 0; i < entries.size(); i++) {
    char word[32];
    std::snprintf(word, sizeof(word), "syn_word_%06u", i);
    const std::string resolved = Dictionary::resolveAltForm(word, cache.c_str());
    if (resolved != entries[i].word) {
      if (misses == 0) firstMiss = word;
      misses++;
    }
  }
  EXPECT_EQ(misses, 0u) << "first miss: " << firstMiss << " (" << misses << "/" << entries.size() << " total)";
}

TEST(DictResolveAltForm, HasAltFormsReturnsCorrectStatus) {
  const std::string cache = cacheDirPointingAtFixture();
  EXPECT_TRUE(Dictionary::hasAltForms(cache.c_str()));

  const std::string stem = fixtureCopyStem("dict_engine_without_syn");
  std::filesystem::remove(stem + ".syn.fpi");
  std::filesystem::remove(stem + ".syn");
  const std::string cache_no_syn = cacheDirPointingAt(stem, "dict_engine_without_syn_cache");
  EXPECT_FALSE(Dictionary::hasAltForms(cache_no_syn.c_str()));
}

TEST(DictResolveAltForm, ResolvesSynonymToHeadword) {
  const std::string cache = cacheDirPointingAtFixture();
  EXPECT_EQ(Dictionary::resolveAltForm("automobile", cache.c_str()), "car");
  EXPECT_EQ(Dictionary::resolveAltForm("auto", cache.c_str()), "car");
}

TEST(DictResolveAltForm, ReturnsEmptyForUnknownWord) {
  const std::string cache = cacheDirPointingAtFixture();
  EXPECT_EQ(Dictionary::resolveAltForm("not_a_synonym", cache.c_str()), "");
}

TEST(DictResolveAltForm, FallsBackToLinearScanWhenFpiMissing) {
  const std::string stem = fixtureCopyStem("dict_engine_without_syn_accel");
  std::filesystem::remove(stem + ".idx.fpi");
  std::filesystem::remove(stem + ".syn.fpi");
  const std::string cache = cacheDirPointingAt(stem, "dict_engine_without_syn_accel_cache");
  EXPECT_EQ(Dictionary::resolveAltForm("automobile", cache.c_str()), "car");
}

// The .fpi bracket search (binarySearchFpi on .idx.fpi) narrows findSimilar's scan
// window around scanStart. For a word deep in a multi-page dictionary, scanStart is
// well past offset 0, so the candidates findSimilar returns are exactly those
// inside the ±7-bracket window around the target's fence group. We assert every
// suggestion is numerically clustered around the target headword: a broken bracket
// search (e.g. landing on the wrong region, or always offset 0) would scan a
// different region and return words from elsewhere in the index, failing this
// band check.

// Parse the trailing NNNNN of a "fuzzy_word_NNNNN" headword (-1 if malformed).
int fuzzyOrdinal(const std::string& w) {
  const std::string prefix = "fuzzy_word_";
  if (w.size() <= prefix.size() || w.compare(0, prefix.size(), prefix) != 0) return -1;
  return std::atoi(w.c_str() + prefix.size());
}

void expectSuggestionsClusteredNear(int target) {
  const std::string cache = cacheDirPointingAt(kFuzzyStem, "dict_engine_fuzzy");
  char query[32];
  std::snprintf(query, sizeof(query), "fuzzy_word_%05d", target);
  const auto results = Dictionary::findSimilar(query, 6, cache.c_str());
  ASSERT_FALSE(results.empty()) << "expected suggestions near " << query;
  // Window is ~±7 fence groups (~±224 ordinals) around the target's group;
  // ±400 is a generous bound that still excludes a wrong-region (e.g. offset-0) scan.
  for (const auto& r : results) {
    const int ord = fuzzyOrdinal(r);
    ASSERT_GE(ord, 0) << "unexpected headword form: " << r;
    EXPECT_LE(std::abs(ord - target), 400) << "suggestion " << r << " is outside the scan window around " << target;
  }
}

TEST(DictFindSimilarFpi, SuggestionsClusteredDeepInMultiPageDict) {
  expectSuggestionsClusteredNear(1400);  // page ~43 of ~47
}

TEST(DictFindSimilarFpi, SuggestionsClusteredMidMultiPageDict) {
  expectSuggestionsClusteredNear(750);  // page ~23 of ~47
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

TEST(DictZipTest, LookupPrepMiniDefinitions) {
  const std::string stem = std::string(DICT_FIXTURE_DIR) + "/prep-mini/prep-mini";
  const std::string cache = cacheDirPointingAt(stem, "dict_engine_prep_mini");

  const auto entries = parseIdxFile(stem + ".idx");
  ASSERT_FALSE(entries.empty());

  for (const auto& e : entries) {
    std::string def = Dictionary::lookup(e.word, {}, cache.c_str());
    EXPECT_FALSE(def.empty()) << "Failed to lookup definition for " << e.word;
  }
}

// Host tests for the dictionary lookup text helpers: word trimming and
// headword ranking for multi-entry results.

#include <gtest/gtest.h>

#include <string>

#include "DictWordUtils.h"

using namespace DictWordUtils;

namespace {

TEST(DictWordUtils, TrimsPunctuationAndKeepsCase) {
  EXPECT_EQ(trimWordEdges("Laconic,"), "Laconic");
  EXPECT_EQ(trimWordEdges("\xE2\x80\x9Cgarage.\xE2\x80\x9D"), "garage");
  EXPECT_EQ(trimWordEdges("(well-known)"), "well-known");
  EXPECT_EQ(trimWordEdges("caf\xC3\xA9!"), "caf\xC3\xA9");
  EXPECT_EQ(trimWordEdges("--"), "");
  EXPECT_EQ(trimWordEdges(nullptr), "");
}

TEST(DictWordUtils, RanksExactCaseThenLowercase) {
  EXPECT_LT(headwordRank("laconic", "laconic"), headwordRank("Laconic", "laconic"));
  EXPECT_LT(headwordRank("Laconic", "Laconic"), headwordRank("laconic", "Laconic"));
  // No exact match (stemmed lookup): lowercase headwords still come first.
  EXPECT_LT(headwordRank("laconic", "laconics"), headwordRank("Laconic", "laconics"));
}

}  // namespace

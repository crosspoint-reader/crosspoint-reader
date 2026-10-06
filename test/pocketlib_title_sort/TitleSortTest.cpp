// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "LibraryFormat.h"
#include "LibraryText.h"
#include "TitleSortKey.h"

namespace {

std::string key(const std::string& title) {
  const std::string folded = library::fold(title);
  return std::string(library::titleSortKey(folded));
}

}  // namespace

TEST(TitleSortKey, LeadingArticlesAreSkipped) {
  EXPECT_EQ(key("The Road"), "road");
  EXPECT_EQ(key("A Farewell to Arms"), "farewell to arms");
  EXPECT_EQ(key("An Instance of the Fingerpost"), "instance of the fingerpost");
  EXPECT_EQ(key("THE SHINING"), "shining");
}

TEST(TitleSortKey, OnlyAWholeLeadingWordCounts) {
  EXPECT_EQ(key("Theodore Rex"), "theodore rex");
  EXPECT_EQ(key("Anathem"), "anathem");
  EXPECT_EQ(key("Atonement"), "atonement");
  EXPECT_EQ(key("Brave New World, The"), "brave new world the");
}

TEST(TitleSortKey, ABookCalledTheIsLeftAlone) {
  EXPECT_EQ(key("The"), "the");
  EXPECT_EQ(key("A"), "a");
}

TEST(TitleSortKey, ALetterIsNotAnArticle) {
  EXPECT_EQ(key("A Is for Alibi"), "a is for alibi");
  EXPECT_EQ(key("A Series of Unfortunate Events"), "series of unfortunate events");
}

TEST(TitleSortKey, ShelfOrder) {
  std::vector<std::string> titles = {"The Road", "Atonement", "A Wrinkle in Time", "Dune", "The Hobbit"};
  std::sort(titles.begin(), titles.end(), [](const std::string& a, const std::string& b) { return key(a) < key(b); });
  EXPECT_EQ(titles, (std::vector<std::string>{"Atonement", "Dune", "The Hobbit", "The Road", "A Wrinkle in Time"}));
}

TEST(TitleSortKey, IndexesBuiltBeforeTheChangeAreRebuilt) { EXPECT_EQ(library::CLIX_FOLD_VERSION, 5); }

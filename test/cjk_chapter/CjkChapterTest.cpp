#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include "IsHasChapterPattern.h"

TEST(CjkChapter, DetectsStandardChapter) {
  const std::string s = "第一章 名字";
  EXPECT_TRUE(isHasChapterPattern(s.c_str(), static_cast<int>(s.size())));
}

TEST(CjkChapter, DetectsNumberedChapter) {
  const std::string s = "第十二章 大战";
  EXPECT_TRUE(isHasChapterPattern(s.c_str(), static_cast<int>(s.size())));
}

TEST(CjkChapter, RejectsEnglish) {
  const std::string s = "Chapter 1: The Beginning";
  EXPECT_FALSE(isHasChapterPattern(s.c_str(), static_cast<int>(s.size())));
}

TEST(CjkChapter, RejectsOnlyDi) {
  const std::string s = "第一段";
  EXPECT_FALSE(isHasChapterPattern(s.c_str(), static_cast<int>(s.size())));
}

TEST(CjkChapter, DetectsAcrossLine) {
  const std::string s = "前言 介绍 第一章 详细";
  EXPECT_TRUE(isHasChapterPattern(s.c_str(), static_cast<int>(s.size())));
}

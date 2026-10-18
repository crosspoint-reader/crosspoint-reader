// textWrap::wrapLines, GfxRenderer::wrappedText's line filling: spaces as before, and the CJK
// line-break rules for text that writes none.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "CjkLineBreak.h"
#include "TextWrap.h"
#include "Utf8.h"

namespace {

// Every character is 10 px wide, so a width is a character count.
constexpr int CHAR_PX = 10;

int codepoints(const std::string& s) {
  int count = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  while (utf8NextCodepoint(&p)) ++count;
  return count;
}

int width(const std::string& s) { return codepoints(s) * CHAR_PX; }

// What truncatedText does with a fixed width: as many characters as fit with the ellipsis.
std::string truncateTo(const std::string& s, const int maxWidth) {
  if (width(s) <= maxWidth) return s;
  std::string out;
  const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  while (true) {
    const auto* before = p;
    if (!utf8NextCodepoint(&p)) break;
    const std::string next = out + std::string(reinterpret_cast<const char*>(before), p - before);
    if (width(next) + CHAR_PX > maxWidth) break;
    out = next;
  }
  return out + "\xE2\x80\xA6";
}

std::vector<std::string> wrap(const char* text, const int maxWidth, const int maxLines,
                              const textWrap::WordBreaker breaker = &CjkLineBreak::breakByteOffsets) {
  return textWrap::wrapLines(
      text, maxWidth, maxLines, breaker, [](const std::string& s) { return width(s); },
      [maxWidth](const std::string& s) { return truncateTo(s, maxWidth); });
}

using Lines = std::vector<std::string>;

TEST(TextWrap, LatinBreaksAtSpacesWithOrWithoutTheBreaker) {
  const char* text = "The quick brown fox jumps over";
  const Lines expected = {"The quick", "brown fox", "jumps over"};
  EXPECT_EQ(wrap(text, 100, 5), expected);
  EXPECT_EQ(wrap(text, 100, 5, nullptr), expected);
}

TEST(TextWrap, ChineseWithoutSpacesBreaksBetweenCharacters) {
  EXPECT_EQ(wrap("请重新启动设备", 30, 5), (Lines{"请重新", "启动设", "备"}));
}

TEST(TextWrap, WithoutTheBreakerASpacelessSentenceIsOneWordCutOnItsFirstLine) {
  EXPECT_EQ(wrap("请重新启动设备", 30, 5, nullptr), (Lines{"请重\xE2\x80\xA6"}));
}

TEST(TextWrap, ClosingPunctuationNeverStartsALine) {
  // Five fit; the full stop may not start the next line, so the fifth character goes with it.
  EXPECT_EQ(wrap("一二三四五。", 50, 5), (Lines{"一二三四", "五。"}));
  EXPECT_EQ(wrap("设置已保存，请重启。", 50, 5), (Lines{"设置已保", "存，请重", "启。"}));
}

TEST(TextWrap, SpacesAroundLatinInCjkTextAreKept) {
  EXPECT_EQ(wrap("打开 Wi-Fi 设置", 200, 3), (Lines{"打开 Wi-Fi 设置"}));
  EXPECT_EQ(wrap("打开 Wi-Fi 设置", 70, 3), (Lines{"打开", "Wi-Fi 设", "置"}));
}

TEST(TextWrap, KoreanBreaksAtItsSpacesOnly) {
  EXPECT_EQ(wrap("설정을 저장했습니다", 60, 5), (Lines{"설정을", "저장했습니다"}));
}

TEST(TextWrap, TheLastLineGetsTheRestJoinedAsWrittenAndTruncated) {
  // Pieces split at CJK breaks rejoin without spaces; a word break keeps its space.
  EXPECT_EQ(wrap("一二三四五六七八九", 40, 2), (Lines{"一二三四", "五六七\xE2\x80\xA6"}));
  EXPECT_EQ(wrap("一二三 四五六七八", 40, 2), (Lines{"一二三", "四五六\xE2\x80\xA6"}));
}

TEST(TextWrap, EveryLineFitsAndTheTextSurvivesWhole) {
  const std::string text = "阅读进度已同步到服务器，下次打开时将从这里继续。";
  const Lines lines = wrap(text.c_str(), 70, 20);
  std::string joined;
  for (const std::string& line : lines) {
    EXPECT_LE(width(line), 70) << line;
    joined += line;
  }
  EXPECT_EQ(joined, text);
}

}  // namespace

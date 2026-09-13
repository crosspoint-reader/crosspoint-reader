#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Utf8.h"
#include "lib/GfxRenderer/TextWrap.h"

namespace {

using Lines = std::vector<std::string>;

// Mock width: one unit per codepoint (so maxWidth is "codepoints per line").
int codepointWidth(const std::string& s) {
  int n = 0;
  auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  while (*p) {
    if (!utf8NextCodepoint(&p)) break;
    ++n;
  }
  return n;
}

// First n codepoints of s (UTF-8 safe).
std::string firstCodepoints(const std::string& s, int n) {
  auto* base = reinterpret_cast<const unsigned char*>(s.c_str());
  auto* p = base;
  while (n-- > 0 && *p) utf8NextCodepoint(&p);
  return s.substr(0, static_cast<size_t>(reinterpret_cast<const char*>(p) - reinterpret_cast<const char*>(base)));
}

// Wrap with the codepoint mock; "~" stands in for the ellipsis (width 1).
Lines wrap(const char* text, int maxWidth, int maxLines) {
  return textwrap::wrapLines(text, maxWidth, maxLines, codepointWidth, [&](const std::string& s) {
    return codepointWidth(s) <= maxWidth ? s : firstCodepoints(s, maxWidth - 1) + "~";
  });
}

}  // namespace

// Ordinary space-separated text wraps on spaces exactly as before.
TEST(TextWrap, NormalWordWrapUnchanged) {
  EXPECT_EQ(wrap("the quick brown fox", 9, 4), (Lines{"the quick", "brown fox"}));
}

TEST(TextWrap, ShortTextOneLine) { EXPECT_EQ(wrap("hello", 10, 3), (Lines{"hello"})); }

// The #2949 bug: a single token wider than the line used to be truncated to an
// ellipsis and the rest dropped. It must now hard-break and keep all the text.
TEST(TextWrap, LongTokenHardBreaksInsteadOfDropping) {
  EXPECT_EQ(wrap("AAAAAAAAAAAAAAAA", 5, 4), (Lines{"AAAAA", "AAAAA", "AAAAA", "A"}));
}

// A long token followed by a normal word: break the token, then flow the word.
TEST(TextWrap, LongTokenThenWordFlows) {
  EXPECT_EQ(wrap("supercalifragilistic ok", 6, 4), (Lines{"superc", "alifra", "gilist", "ic ok"}));
}

// A short word first, then a long token: exercises the flush-and-requeue path
// (the started line is flushed and the over-long word is re-handled fresh).
TEST(TextWrap, ShortWordThenLongTokenRequeues) {
  EXPECT_EQ(wrap("ok supercalifragilistic", 6, 4), (Lines{"ok", "superc", "alifra", "gilis~"}));
}

// maxLines is respected and only the final line is ellipsized. The mock
// ellipsis keeps maxWidth-1 codepoints then "~", so line 2 is "bbb~".
TEST(TextWrap, MaxLinesEllipsizesFinalLine) {
  EXPECT_EQ(wrap("aaaa bbbb cccc dddd eeee", 4, 2), (Lines{"aaaa", "bbb~"}));
}

// A hard-broken token that runs past maxLines ellipsizes the last line and stops.
TEST(TextWrap, LongTokenHittingMaxLinesEllipsizesAndStops) {
  EXPECT_EQ(wrap("AAAAAAAAAAAA end", 5, 3), (Lines{"AAAAA", "AAAAA", "AA e~"}));
}

// Multi-byte scripts (no spaces): hard-break must land on codepoint boundaries
// so every emitted line is valid UTF-8, and no character is dropped.
TEST(TextWrap, CjkHardBreakOnCodepointBoundaries) {
  // 7 CJK codepoints (3 bytes each), 3 per line.
  EXPECT_EQ(wrap("日本語テキスト", 3, 5), (Lines{"日本語", "テキス", "ト"}));
}

// Progress guarantee: even when a single glyph is wider than the whole line, it
// is emitted on its own line rather than looping forever or being dropped.
TEST(TextWrap, GlyphWiderThanLineStillEmitted) {
  // 'W' has width 2; everything else width 1; line width 1.
  auto measure = [](const std::string& s) {
    int w = 0;
    for (char c : s) w += (c == 'W') ? 2 : 1;
    return w;
  };
  auto trunc = [&](const std::string& s) { return s; };
  const Lines out = textwrap::wrapLines("WxW", 1, 5, measure, trunc);
  EXPECT_EQ(out, (Lines{"W", "x", "W"}));
}

// A forced single-glyph break that consumes a whole word must swallow the
// following space, not emit it as an empty line on the next line.
TEST(TextWrap, ForcedBreakConsumingWordSwallowsSeparator) {
  // 'W' has width 2; everything else width 1; line width 1.
  auto measure = [](const std::string& s) {
    int w = 0;
    for (char c : s) w += (c == 'W') ? 2 : 1;
    return w;
  };
  auto trunc = [&](const std::string& s) { return s; };
  const Lines out = textwrap::wrapLines("W xy", 1, 5, measure, trunc);
  EXPECT_EQ(out, (Lines{"W", "x", "y"}));
}

// Separator runs must not start a line: a zero-length word between two spaces
// would otherwise be flushed as an empty line.
TEST(TextWrap, RepeatedSpacesDoNotEmitEmptyLines) {
  EXPECT_EQ(wrap("aaaa  bbbb", 4, 5), (Lines{"aaaa", "bbbb"}));
  EXPECT_EQ(wrap("aa   bb", 4, 5), (Lines{"aa", "bb"}));
}

TEST(TextWrap, LeadingAndTrailingSpacesAreDropped) {
  EXPECT_EQ(wrap(" aaaa", 4, 5), (Lines{"aaaa"}));
  EXPECT_EQ(wrap("  aaaa", 4, 5), (Lines{"aaaa"}));
  EXPECT_EQ(wrap("aaaa ", 4, 5), (Lines{"aaaa"}));
  EXPECT_TRUE(wrap("   ", 4, 5).empty());
}

TEST(TextWrap, EmptyAndNullInputs) {
  EXPECT_TRUE(wrap("", 10, 3).empty());
  EXPECT_TRUE(wrap(nullptr, 10, 3).empty());
  EXPECT_TRUE(wrap("hello", 0, 3).empty());
  EXPECT_TRUE(wrap("hello", 10, 0).empty());
}

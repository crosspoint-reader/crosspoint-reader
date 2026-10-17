// TextBlock's range constructor must be a drop-in for the vector constructor: layout hands it a
// [first, first+count) window over the block's word arrays instead of three freshly copied
// per-line vectors, so a line costs one allocation (the arena) instead of four.
//
// The pipeline goldens already prove the two agree on every corpus book, but no corpus fixture
// contains a literal soft hyphen (U+00AD) — and stripping those during the arena copy is the one
// piece of work the vector path did separately, on a mutable copy, before construction. These
// tests cover that branch directly plus the range bookkeeping the goldens cannot reach.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Epub/TokenBoundary.h"
#include "Epub/blocks/TextBlock.h"

namespace {

constexpr const char* SHY = "\xC2\xAD";  // UTF-8 U+00AD

TextBlock::WordRange makeRange(const std::vector<std::string>& words, const std::vector<EpdFontFamily::Style>& styles,
                               const std::vector<uint8_t>& sizes, const size_t first, const size_t count) {
  TextBlock::WordRange r;
  r.words = &words;
  r.styles = &styles;
  r.sizes = &sizes;
  r.first = first;
  r.count = count;
  return r;
}

// Everything a resident line exposes, so equivalence is checked on the whole surface rather
// than on whichever accessor a single test happened to call.
std::string describe(const TextBlock& b) {
  std::string out = "valid=" + std::to_string(b.valid()) + " words=" + std::to_string(b.wordCount()) +
                    " sizes=" + std::to_string(b.hasWordSizes());
  for (uint16_t i = 0; i < b.wordCount(); i++) {
    out += "|" + std::string(b.wordText(i)) + ",len=" + std::to_string(b.wordTextLen(i)) +
           ",x=" + std::to_string(b.wordXpos(i)) + ",s=" + std::to_string(static_cast<int>(b.wordStyle(i))) +
           ",z=" + std::to_string(b.wordSizePct(i));
  }
  return out;
}

// The vector constructor takes ownership, so each comparison needs its own copies.
std::string viaVectorCtor(const std::vector<std::string>& words, const std::vector<EpdFontFamily::Style>& styles,
                          const std::vector<uint8_t>& sizes, const std::vector<int16_t>& xpos, const size_t first,
                          const size_t count) {
  std::vector<std::string> w(words.begin() + first, words.begin() + first + count);
  // The vector path stripped soft hyphens itself before constructing.
  for (auto& s : w) {
    size_t pos = 0;
    while ((pos = s.find(SHY, pos)) != std::string::npos) s.erase(pos, 2);
  }
  std::vector<EpdFontFamily::Style> st(styles.begin() + first, styles.begin() + first + count);
  std::vector<uint8_t> sz(sizes.begin() + first, sizes.begin() + first + count);
  TextBlock block(std::move(w), xpos, std::move(st), BlockStyle(), std::move(sz));
  return describe(block);
}

std::string viaRangeCtor(const std::vector<std::string>& words, const std::vector<EpdFontFamily::Style>& styles,
                         const std::vector<uint8_t>& sizes, const std::vector<int16_t>& xpos, const size_t first,
                         const size_t count) {
  TextBlock block(makeRange(words, styles, sizes, first, count), xpos, BlockStyle());
  return describe(block);
}

struct Fixture {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> styles;
  std::vector<uint8_t> sizes;
};

Fixture uniformFixture() {
  return {
      {"alpha", "beta", "gamma", "delta", "epsilon"},
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD},
      {100, 100, 100, 100, 100}};
}

TEST(TextBlockRange, MatchesVectorCtorForInteriorSlice) {
  const Fixture f = uniformFixture();
  const std::vector<int16_t> xpos{0, 40, 90};
  // A middle window: catches any place that indexes the source from 0 instead of `first`.
  EXPECT_EQ(viaVectorCtor(f.words, f.styles, f.sizes, xpos, 1, 3),
            viaRangeCtor(f.words, f.styles, f.sizes, xpos, 1, 3));
}

TEST(TextBlockRange, MatchesVectorCtorForTrailingSlice) {
  const Fixture f = uniformFixture();
  const std::vector<int16_t> xpos{0, 55};
  EXPECT_EQ(viaVectorCtor(f.words, f.styles, f.sizes, xpos, 3, 2),
            viaRangeCtor(f.words, f.styles, f.sizes, xpos, 3, 2));
}

TEST(TextBlockRange, StripsSoftHyphensAndMatchesVectorCtor) {
  Fixture f;
  f.words = {std::string("hy") + SHY + "phen", std::string(SHY) + "leading", std::string("trailing") + SHY,
             std::string("mul") + SHY + "ti" + SHY + "ple", "plain"};
  f.styles.assign(f.words.size(), EpdFontFamily::REGULAR);
  f.sizes.assign(f.words.size(), 100);
  const std::vector<int16_t> xpos{0, 30, 60, 90, 120};

  EXPECT_EQ(viaVectorCtor(f.words, f.styles, f.sizes, xpos, 0, 5),
            viaRangeCtor(f.words, f.styles, f.sizes, xpos, 0, 5));

  // Explicit expectations too, so a shared bug in both paths cannot pass by agreeing.
  TextBlock block(makeRange(f.words, f.styles, f.sizes, 0, 5), xpos, BlockStyle());
  ASSERT_TRUE(block.valid());
  EXPECT_STREQ(block.wordText(0), "hyphen");
  EXPECT_STREQ(block.wordText(1), "leading");
  EXPECT_STREQ(block.wordText(2), "trailing");
  EXPECT_STREQ(block.wordText(3), "multiple");
  EXPECT_STREQ(block.wordText(4), "plain");
  // wordTextLen must reflect the STRIPPED length, since it is derived from arena offsets.
  EXPECT_EQ(block.wordTextLen(0), 6);
  EXPECT_EQ(block.wordTextLen(3), 8);
}

TEST(TextBlockRange, WordEntirelySoftHyphensBecomesEmpty) {
  Fixture f;
  f.words = {std::string(SHY) + SHY, "after"};
  f.styles.assign(2, EpdFontFamily::REGULAR);
  f.sizes.assign(2, 100);
  const std::vector<int16_t> xpos{0, 10};

  TextBlock block(makeRange(f.words, f.styles, f.sizes, 0, 2), xpos, BlockStyle());
  ASSERT_TRUE(block.valid());
  EXPECT_STREQ(block.wordText(0), "");
  EXPECT_EQ(block.wordTextLen(0), 0);
  EXPECT_STREQ(block.wordText(1), "after");  // the NUL bookkeeping must survive an empty word
}

TEST(TextBlockRange, NonUniformSizesArePreservedForTheSliceOnly) {
  Fixture f = uniformFixture();
  f.sizes = {100, 150, 100, 100, 100};
  const std::vector<int16_t> xpos{0, 40};

  // Slice covering the non-100 word keeps the sizes array...
  TextBlock withSizes(makeRange(f.words, f.styles, f.sizes, 1, 2), xpos, BlockStyle());
  ASSERT_TRUE(withSizes.valid());
  EXPECT_TRUE(withSizes.hasWordSizes());
  EXPECT_EQ(withSizes.wordSizePct(0), 150);
  EXPECT_EQ(withSizes.wordSizePct(1), 100);

  // ...while a slice that misses it normalizes to "no sizes", exactly like the vector ctor,
  // so uniform lines keep the zero-cost fast paths and the cache bytes stay identical.
  TextBlock uniform(makeRange(f.words, f.styles, f.sizes, 2, 2), xpos, BlockStyle());
  ASSERT_TRUE(uniform.valid());
  EXPECT_FALSE(uniform.hasWordSizes());
  EXPECT_EQ(uniform.wordSizePct(0), 100);
  EXPECT_EQ(viaVectorCtor(f.words, f.styles, f.sizes, xpos, 2, 2),
            viaRangeCtor(f.words, f.styles, f.sizes, xpos, 2, 2));
}

TEST(TextBlockRange, EmptyRangeIsValidAndEmpty) {
  const Fixture f = uniformFixture();
  TextBlock block(makeRange(f.words, f.styles, f.sizes, 2, 0), {}, BlockStyle());
  EXPECT_TRUE(block.valid());
  EXPECT_EQ(block.wordCount(), 0);
}

TEST(TextBlockRange, RejectsOutOfBoundsRange) {
  const Fixture f = uniformFixture();
  const std::vector<int16_t> xpos{0, 10, 20};
  // first + count runs past the source arrays.
  TextBlock block(makeRange(f.words, f.styles, f.sizes, 3, 3), xpos, BlockStyle());
  EXPECT_FALSE(block.valid());
}

TEST(TextBlockRange, RejectsXposCountMismatch) {
  const Fixture f = uniformFixture();
  const std::vector<int16_t> xpos{0, 10};  // 2 entries for a 3-word range
  TextBlock block(makeRange(f.words, f.styles, f.sizes, 0, 3), xpos, BlockStyle());
  EXPECT_FALSE(block.valid());
}

TEST(TextBlockRange, RejectsNullArrays) {
  const Fixture f = uniformFixture();
  TextBlock::WordRange r;
  r.words = nullptr;
  r.styles = &f.styles;
  r.count = 1;
  TextBlock block(r, {0}, BlockStyle());
  EXPECT_FALSE(block.valid());
}

TEST(TextBlockRange, NullSizesMeansUniform) {
  const Fixture f = uniformFixture();
  TextBlock::WordRange r;
  r.words = &f.words;
  r.styles = &f.styles;
  r.sizes = nullptr;  // layout may not track per-word sizes at all
  r.first = 0;
  r.count = 2;
  TextBlock block(r, {0, 30}, BlockStyle());
  ASSERT_TRUE(block.valid());
  EXPECT_FALSE(block.hasWordSizes());
  EXPECT_EQ(block.wordSizePct(1), 100);
}

// The continuation flag rides in the spare high bit of the packed style byte. It is what tells
// the dictionary overlay that "rea" and "ding" are one word rather than two, so it has to
// survive the range copy without leaking into the style the renderer reads back.
TEST(TextBlockContinues, RangeCtorCarriesTheFlagWithoutDisturbingStyles) {
  const std::vector<std::string> words = {"noun", "rea", "ding", "?", "next"};
  const std::vector<EpdFontFamily::Style> styles = {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::REGULAR,
                                                    EpdFontFamily::REGULAR, EpdFontFamily::ITALIC};
  const std::vector<uint8_t> sizes(words.size(), 100);
  const std::vector<TokenBoundary::Boundary> boundaries = {
      {false, false}, {false, false}, {true, false}, {true, false}, {false, false}};
  TextBlock::WordRange range = makeRange(words, styles, sizes, 0, words.size());
  range.boundaries = &boundaries;

  TextBlock block(range, {0, 40, 70, 110, 130}, BlockStyle());
  ASSERT_TRUE(block.valid());
  for (uint16_t i = 0; i < words.size(); i++) {
    EXPECT_EQ(block.wordContinues(i), boundaries[i].continues) << "word " << i;
    EXPECT_EQ(block.wordStyle(i), styles[i]) << "word " << i;
  }
}

// A style that uses the top of the 7-bit range must not be mistaken for a continuation, and a
// continuation must not smuggle a bit into the style.
TEST(TextBlockContinues, HighStyleBitsSurviveAlongsideTheFlag) {
  const std::vector<std::string> words = {"small", "caps"};
  const auto smallCapsBold = static_cast<EpdFontFamily::Style>(EpdFontFamily::SMALL_CAPS | EpdFontFamily::BOLD |
                                                               EpdFontFamily::SUP | EpdFontFamily::UNDERLINE);
  const std::vector<EpdFontFamily::Style> styles = {smallCapsBold, smallCapsBold};
  const std::vector<uint8_t> sizes(words.size(), 100);
  const std::vector<TokenBoundary::Boundary> boundaries = {{false, false}, {true, false}};
  TextBlock::WordRange range = makeRange(words, styles, sizes, 0, words.size());
  range.boundaries = &boundaries;

  TextBlock block(range, {0, 50}, BlockStyle());
  ASSERT_TRUE(block.valid());
  EXPECT_EQ(block.wordStyle(0), smallCapsBold);
  EXPECT_EQ(block.wordStyle(1), smallCapsBold);
  EXPECT_FALSE(block.wordContinues(0));
  EXPECT_TRUE(block.wordContinues(1));
}

// The flags are indexed like the source words, not from 0 -- an interior line must report the
// flags of the words it actually holds.
TEST(TextBlockContinues, FlagsFollowTheRangeWindow) {
  const Fixture f = uniformFixture();
  const std::vector<TokenBoundary::Boundary> boundaries = {
      {false, false}, {false, false}, {true, false}, {true, false}, {false, false}};
  TextBlock::WordRange range = makeRange(f.words, f.styles, f.sizes, 2, 2);
  range.boundaries = &boundaries;

  TextBlock block(range, {0, 40}, BlockStyle());
  ASSERT_TRUE(block.valid());
  EXPECT_TRUE(block.wordContinues(0));  // source word 2
  EXPECT_TRUE(block.wordContinues(1));  // source word 3
}

// A caller that does not track continuation (and a cache written before the bit existed) must
// read as "every word is space-separated".
TEST(TextBlockContinues, AbsentFlagsMeanNoContinuation) {
  const Fixture f = uniformFixture();
  TextBlock block(makeRange(f.words, f.styles, f.sizes, 0, 3), {0, 40, 90}, BlockStyle());
  ASSERT_TRUE(block.valid());
  for (uint16_t i = 0; i < 3; i++) EXPECT_FALSE(block.wordContinues(i));

  // A short vector is ignored rather than read out of bounds.
  const std::vector<TokenBoundary::Boundary> tooShort = {{true, false}};
  TextBlock::WordRange range = makeRange(f.words, f.styles, f.sizes, 0, 3);
  range.boundaries = &tooShort;
  TextBlock guarded(range, {0, 40, 90}, BlockStyle());
  ASSERT_TRUE(guarded.valid());
  for (uint16_t i = 0; i < 3; i++) EXPECT_FALSE(guarded.wordContinues(i));
}

// A CJK gap is a break opportunity, so the block must not record it as a continuation: the dictionary
// would otherwise join every character of a CJK run into one "word".
TEST(TextBlockContinues, ACjkGapIsNotAContinuation) {
  const Fixture f = uniformFixture();
  const std::vector<TokenBoundary::Boundary> boundaries = {{false, false}, {false, true}, {true, false}};
  TextBlock::WordRange range = makeRange(f.words, f.styles, f.sizes, 0, 3);
  range.boundaries = &boundaries;
  TextBlock block(range, {0, 40, 90}, BlockStyle());
  ASSERT_TRUE(block.valid());
  EXPECT_FALSE(block.wordContinues(1));  // the CJK gap
  EXPECT_TRUE(block.wordContinues(2));   // glued
}

}  // namespace

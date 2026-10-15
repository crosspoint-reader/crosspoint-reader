// WordSizeLayoutTest.cpp
//
// Compiles the REAL ParsedText and TextBlock against a fixed-width GfxRenderer
// shim (see GfxRenderer.h in this directory) and verifies the per-word inline
// font-size channel introduced for size-aware rendering:
//
//   - addWord() clamps and carries sizePct into the TextBlock lines
//   - all-100% lines normalize to an empty wordSizes vector (zero-cost path)
//   - word measurement scales with the word's own size, changing line breaks
//   - hyphenation word splits keep the sizes vector in lockstep
//   - TextBlock serialization round-trips sizes (and stays 1 byte for uniform lines)
//   - render baseline-aligns mixed-size words (smaller words shift down)

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Epub/FontSizeLadder.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;

struct LayoutResult {
  std::vector<std::unique_ptr<TextBlock>> lines;
};

// Lay out the given ParsedText at viewportWidth and collect the emitted lines.
LayoutResult layout(ParsedText& text, const GfxRenderer& renderer, const uint16_t viewportWidth) {
  LayoutResult result;
  text.layoutAndExtractLines(
      renderer, kFontId, viewportWidth,
      [&result](std::unique_ptr<TextBlock> line, bool, bool) {
        result.lines.push_back(std::move(line));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  return result;
}

// Suppress the automatic one-em paragraph indent so width math in tests stays simple.
BlockStyle noIndentStyle() {
  BlockStyle bs;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  return bs;
}

}  // namespace

// ---------------------------------------------------------------------------
// Size propagation and normalization
// ---------------------------------------------------------------------------

TEST(WordSizePropagation, SizesReachTextBlockLines) {
  GfxRenderer renderer;
  ParsedText text(/*extraParagraphSpacing=*/false, /*hyphenationEnabled=*/false, noIndentStyle());
  text.addWord("normal", EpdFontFamily::REGULAR);
  text.addWord("small", EpdFontFamily::REGULAR, false, false, 80);
  text.addWord("big", EpdFontFamily::REGULAR, false, false, 150);

  const auto result = layout(text, renderer, 400);
  ASSERT_EQ(result.lines.size(), 1u);
  const auto& line = *result.lines[0];
  ASSERT_TRUE(line.hasWordSizes());
  ASSERT_EQ(line.getWordSizes().size(), 3u);
  EXPECT_EQ(line.getWordSizes()[0], 100);
  EXPECT_EQ(line.getWordSizes()[1], 80);
  EXPECT_EQ(line.getWordSizes()[2], 150);
  EXPECT_EQ(line.maxSizePct(), 150);
}

TEST(WordSizePropagation, AddWordClampsToRange) {
  GfxRenderer renderer;
  ParsedText text(false, false, noIndentStyle());
  text.addWord("tiny", EpdFontFamily::REGULAR, false, false, 10);   // below MIN -> 30
  text.addWord("huge", EpdFontFamily::REGULAR, false, false, 255);  // above MAX -> 250

  const auto result = layout(text, renderer, 800);
  ASSERT_EQ(result.lines.size(), 1u);
  ASSERT_TRUE(result.lines[0]->hasWordSizes());
  EXPECT_EQ(result.lines[0]->getWordSizes()[0], ParsedText::MIN_WORD_SIZE_PCT);
  EXPECT_EQ(result.lines[0]->getWordSizes()[1], ParsedText::MAX_WORD_SIZE_PCT);
}

TEST(WordSizePropagation, UniformLinesNormalizeToEmpty) {
  GfxRenderer renderer;
  ParsedText text(false, false, noIndentStyle());
  text.addWord("all", EpdFontFamily::REGULAR, false, false, 100);
  text.addWord("default", EpdFontFamily::REGULAR);

  const auto result = layout(text, renderer, 400);
  ASSERT_EQ(result.lines.size(), 1u);
  EXPECT_FALSE(result.lines[0]->hasWordSizes());
  EXPECT_EQ(result.lines[0]->maxSizePct(), 100);
}

// ---------------------------------------------------------------------------
// Measurement: an enlarged word takes proportionally more line width
// ---------------------------------------------------------------------------

TEST(WordSizeMeasurement, EnlargedWordForcesEarlierBreak) {
  GfxRenderer renderer;
  // Four 4-glyph words: 4*40 + 3*5 = 175 px -> fits one 175 px line at 100%.
  {
    ParsedText text(false, false, noIndentStyle());
    for (const char* w : {"aaaa", "bbbb", "cccc", "dddd"}) text.addWord(w, EpdFontFamily::REGULAR);
    const auto result = layout(text, renderer, 175);
    EXPECT_EQ(result.lines.size(), 1u);
  }
  // Same words, but one at 200% (80 px instead of 40) no longer fits -> 2 lines.
  {
    ParsedText text(false, false, noIndentStyle());
    text.addWord("aaaa", EpdFontFamily::REGULAR);
    text.addWord("bbbb", EpdFontFamily::REGULAR, false, false, 200);
    text.addWord("cccc", EpdFontFamily::REGULAR);
    text.addWord("dddd", EpdFontFamily::REGULAR);
    const auto result = layout(text, renderer, 175);
    EXPECT_GE(result.lines.size(), 2u);
    // Every emitted line must keep words/sizes vectors in lockstep.
    for (const auto& line : result.lines) {
      if (line->hasWordSizes()) {
        EXPECT_EQ(line->getWordSizes().size(), line->wordCount());
      }
    }
  }
}

TEST(WordSizeMeasurement, ShrunkWordsAllowLaterBreak) {
  GfxRenderer renderer;
  // Five 4-glyph words at 100%: 5*40 + 4*5 = 220 px -> needs 2 lines at 200 px.
  {
    ParsedText text(false, false, noIndentStyle());
    for (const char* w : {"aaaa", "bbbb", "cccc", "dddd", "eeee"}) text.addWord(w, EpdFontFamily::REGULAR);
    const auto result = layout(text, renderer, 200);
    EXPECT_GE(result.lines.size(), 2u);
  }
  // All words at 50% (20 px each): 5*20 + 4*5 = 120 px -> one line.
  {
    ParsedText text(false, false, noIndentStyle());
    for (const char* w : {"aaaa", "bbbb", "cccc", "dddd", "eeee"})
      text.addWord(w, EpdFontFamily::REGULAR, false, false, 50);
    const auto result = layout(text, renderer, 200);
    EXPECT_EQ(result.lines.size(), 1u);
  }
}

// ---------------------------------------------------------------------------
// Word splitting keeps the sizes vector in lockstep
// ---------------------------------------------------------------------------

TEST(WordSizeSplitting, OversizedWordSplitInheritsSize) {
  GfxRenderer renderer;
  ParsedText text(false, /*hyphenationEnabled=*/false, noIndentStyle());
  // 30 glyphs at 50% = 150 px > 100 px viewport -> fallback split must fire.
  text.addWord("abcdefghijklmnopqrstuvwxyzabcd", EpdFontFamily::REGULAR, false, false, 50);

  const auto result = layout(text, renderer, 100);
  ASSERT_GE(result.lines.size(), 2u);
  size_t totalWords = 0;
  for (const auto& line : result.lines) {
    totalWords += line->wordCount();
    ASSERT_TRUE(line->hasWordSizes());
    ASSERT_EQ(line->getWordSizes().size(), line->wordCount());
    for (const uint8_t sz : line->getWordSizes()) {
      EXPECT_EQ(sz, 50);  // every fragment of the split word keeps the original size
    }
  }
  EXPECT_GE(totalWords, 2u);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

TEST(WordSizeSerialization, RoundTripsMixedSizes) {
  TextBlock original({"one", "two", "three"}, {0, 40, 80},
                     {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC}, BlockStyle(),
                     {100, 80, 150});

  FsFile file = HalFile::forReadWrite();
  ASSERT_TRUE(original.serialize(file));
  ASSERT_TRUE(file.seek(0));

  const auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->wordCount(), 3u);
  ASSERT_TRUE(restored->hasWordSizes());
  EXPECT_EQ(restored->getWordSizes(), (std::vector<uint8_t>{100, 80, 150}));
  EXPECT_EQ(std::string(restored->wordText(1)), "two");
}

TEST(WordSizeSerialization, UniformBlockCostsOneFlagByte) {
  TextBlock uniform({"one", "two"}, {0, 40}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, BlockStyle());
  TextBlock mixed({"one", "two"}, {0, 40}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, BlockStyle(), {100, 80});

  FsFile uniformFile = HalFile::forReadWrite();
  FsFile mixedFile = HalFile::forReadWrite();
  ASSERT_TRUE(uniform.serialize(uniformFile));
  ASSERT_TRUE(mixed.serialize(mixedFile));
  // Mixed pays the flag byte plus one byte per word; uniform only the flag byte.
  EXPECT_EQ(mixedFile.size(), uniformFile.size() + 2);

  ASSERT_TRUE(uniformFile.seek(0));
  const auto restored = TextBlock::deserialize(uniformFile);
  ASSERT_NE(restored, nullptr);
  EXPECT_FALSE(restored->hasWordSizes());
}

// The flat arena packs word text, offsets, xpos and styles into one allocation;
// verify every per-word field survives a serialize/deserialize round-trip and
// that wordText() stays NUL-terminated with the right length.
TEST(WordSizeSerialization, ArenaRoundTripPreservesPerWordFields) {
  const std::vector<std::string> words = {"Hello", "wörld", "!", "a"};
  const std::vector<int16_t> xpos = {0, 60, 120, 140};
  const std::vector<EpdFontFamily::Style> styles = {EpdFontFamily::REGULAR, EpdFontFamily::BOLD,
                                                    EpdFontFamily::UNDERLINE, EpdFontFamily::ITALIC};
  TextBlock original(words, xpos, styles, BlockStyle());
  ASSERT_TRUE(original.valid());

  FsFile file = HalFile::forReadWrite();
  ASSERT_TRUE(original.serialize(file));
  ASSERT_TRUE(file.seek(0));

  const auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->wordCount(), words.size());
  EXPECT_FALSE(restored->hasWordSizes());
  for (uint16_t i = 0; i < words.size(); ++i) {
    EXPECT_EQ(std::string(restored->wordText(i)), words[i]);
    EXPECT_EQ(restored->wordTextLen(i), words[i].size());
    EXPECT_EQ(restored->wordXpos(i), xpos[i]);
    EXPECT_EQ(restored->wordStyle(i), styles[i]);
  }
}

// ---------------------------------------------------------------------------
// Word continuation (issue #206: bionic reading and hyphenation split one word)
// ---------------------------------------------------------------------------

// Bionic reading rewrites "reading" into a bold "rea" plus a plain "ding". The pieces have to
// reach the line marked as one word, or the dictionary overlay selects half of it.
TEST(WordContinuation, BionicHalvesAreMarkedAsOneWord) {
  GfxRenderer renderer;
  ParsedText text(false, false, noIndentStyle(), /*bionicReadingEnabled=*/true);
  text.addWord("reading", EpdFontFamily::REGULAR);
  text.addWord("here", EpdFontFamily::REGULAR);

  const auto result = layout(text, renderer, 400);
  ASSERT_EQ(result.lines.size(), 1u);
  const TextBlock& line = *result.lines[0];
  ASSERT_EQ(line.wordCount(), 4);
  EXPECT_STREQ(line.wordText(0), "read");
  EXPECT_STREQ(line.wordText(1), "ing");
  EXPECT_STREQ(line.wordText(2), "he");
  EXPECT_STREQ(line.wordText(3), "re");
  EXPECT_FALSE(line.wordContinues(0));
  EXPECT_TRUE(line.wordContinues(1));
  EXPECT_FALSE(line.wordContinues(2));
  EXPECT_TRUE(line.wordContinues(3));
  // The bold prefix is what makes the two pieces look like separate words on screen; the flag
  // has to survive alongside it.
  EXPECT_EQ(line.wordStyle(0), EpdFontFamily::BOLD);
  EXPECT_EQ(line.wordStyle(1), EpdFontFamily::REGULAR);
}

// Punctuation the parser attaches to a word arrives as its own token with the same flag, so it
// travels with the word instead of becoming a selectable "word" of its own.
TEST(WordContinuation, AttachedPunctuationIsMarkedAsContinuation) {
  GfxRenderer renderer;
  ParsedText text(false, false, noIndentStyle());
  text.addWord("question", EpdFontFamily::REGULAR);
  text.addWord("?", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/true);

  const auto result = layout(text, renderer, 400);
  ASSERT_EQ(result.lines.size(), 1u);
  EXPECT_FALSE(result.lines[0]->wordContinues(0));
  EXPECT_TRUE(result.lines[0]->wordContinues(1));
}

// Hyphenation splits the word ACROSS lines, so the halves land in different blocks and no flag
// can join them. What survives is the shape the overlay keys on: the first line ends on the
// inserted hyphen and the next starts with the rest of the word.
TEST(WordContinuation, HyphenationLeavesTheHyphenOnTheFirstLine) {
  GfxRenderer renderer;
  ParsedText text(false, /*hyphenationEnabled=*/true, noIndentStyle());
  text.addWord("Quadratkilometer", EpdFontFamily::REGULAR);

  // Narrow enough that the single word cannot fit, so the breaker has to split it.
  const auto result = layout(text, renderer, 120);
  ASSERT_GE(result.lines.size(), 2u);
  const TextBlock& first = *result.lines[0];
  const std::string prefix = first.wordText(first.wordCount() - 1);
  ASSERT_FALSE(prefix.empty());
  EXPECT_EQ(prefix.back(), '-');
  EXPECT_FALSE(result.lines[1]->wordContinues(0));  // a new line always starts a new run
  const std::string suffix = result.lines[1]->wordText(0);
  ASSERT_FALSE(suffix.empty());
  // Dropping the inserted hyphen and joining is what reconstructs the word.
  EXPECT_EQ(prefix.substr(0, prefix.size() - 1) + suffix, "Quadratkilometer");
}

// The continuation flag shares the style byte with EpdFontFamily::Style, and the arena is
// written to the section cache verbatim, so the flag has to come back off disk intact.
TEST(WordSizeSerialization, ContinuationFlagRoundTrips) {
  const std::vector<std::string> words = {"rea", "ding", "next"};
  const std::vector<EpdFontFamily::Style> styles = {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::ITALIC};
  TextBlock original(words, {0, 30, 80}, styles, BlockStyle(), {}, {false, true, false});
  ASSERT_TRUE(original.valid());

  FsFile file = HalFile::forReadWrite();
  ASSERT_TRUE(original.serialize(file));
  ASSERT_TRUE(file.seek(0));

  const auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->wordCount(), words.size());
  EXPECT_FALSE(restored->wordContinues(0));
  EXPECT_TRUE(restored->wordContinues(1));
  EXPECT_FALSE(restored->wordContinues(2));
  for (uint16_t i = 0; i < words.size(); ++i) EXPECT_EQ(restored->wordStyle(i), styles[i]);
}

// An empty word ("") is stored as a bare NUL; the offset table must still be
// strictly increasing and validation must accept it.
TEST(WordSizeSerialization, EmptyStringWordRoundTrips) {
  TextBlock original({"", "x", ""}, {0, 10, 20},
                     {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, BlockStyle());
  ASSERT_TRUE(original.valid());

  FsFile file = HalFile::forReadWrite();
  ASSERT_TRUE(original.serialize(file));
  ASSERT_TRUE(file.seek(0));

  const auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->wordCount(), 3u);
  EXPECT_EQ(std::string(restored->wordText(0)), "");
  EXPECT_EQ(restored->wordTextLen(0), 0u);
  EXPECT_EQ(std::string(restored->wordText(1)), "x");
  EXPECT_EQ(std::string(restored->wordText(2)), "");
}

// A zero-word block carries no arena; it must round-trip as a valid empty block.
TEST(WordSizeSerialization, EmptyBlockRoundTrips) {
  TextBlock original({}, {}, {}, BlockStyle());
  ASSERT_TRUE(original.valid());
  EXPECT_TRUE(original.isEmpty());

  FsFile file = HalFile::forReadWrite();
  ASSERT_TRUE(original.serialize(file));
  ASSERT_TRUE(file.seek(0));

  const auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->wordCount(), 0u);
  EXPECT_TRUE(restored->isEmpty());
}

// ---------------------------------------------------------------------------
// FontSizeLadder: snapping an effective block size to real sibling fonts
// ---------------------------------------------------------------------------

namespace {
// Bookerly body at 14 pt: rungs 10/12/14/16/18 pt as percent of the body.
FontSizeLadder bookerly14Ladder() {
  FontSizeLadder ladder;
  ladder.addRung(101, 71);   // 10 pt
  ladder.addRung(102, 85);   // 12 pt
  ladder.addRung(103, 100);  // 14 pt (body)
  ladder.addRung(104, 114);  // 16 pt
  ladder.addRung(105, 128);  // 18 pt
  return ladder;
}
}  // namespace

TEST(FontSizeLadderTest, EmptyLadderKeepsScaleFallback) {
  const FontSizeLadder empty;
  const auto r = empty.resolve(160.0f);
  EXPECT_EQ(r.fontId, 0);
  EXPECT_FLOAT_EQ(r.residual, 1.6f);
}

TEST(FontSizeLadderTest, SnapsToNearestRungWithResidual) {
  const auto ladder = bookerly14Ladder();
  // h1 default 160% → nearest rung is 18 pt (128%), residual 160/128.
  const auto h1 = ladder.resolve(160.0f);
  EXPECT_EQ(h1.fontId, 105);
  EXPECT_FLOAT_EQ(h1.residual, 160.0f / 128.0f);
  // 80% (footnote) → nearest rung is 12 pt (85%), residual < 1.
  const auto small = ladder.resolve(80.0f);
  EXPECT_EQ(small.fontId, 102);
  EXPECT_FLOAT_EQ(small.residual, 80.0f / 85.0f);
  // Exact rung hit → residual 1.0.
  const auto exact = ladder.resolve(114.0f);
  EXPECT_EQ(exact.fontId, 104);
  EXPECT_FLOAT_EQ(exact.residual, 1.0f);
}

TEST(FontSizeLadderTest, NearBodySizesStayOnBodyFont) {
  const auto ladder = bookerly14Ladder();
  // 105% is nearest the 100% body rung → fontId 0, pure scale (legacy path).
  const auto near = ladder.resolve(105.0f);
  EXPECT_EQ(near.fontId, 0);
  EXPECT_FLOAT_EQ(near.residual, 1.05f);
}

TEST(FontSizeLadderTest, ResidualDeadZoneSnapsToNativeGlyphs) {
  const auto ladder = bookerly14Ladder();
  // 87% → 12 pt rung (85%), residual 1.024 — inside the 3% zone → native 12 pt.
  const auto near12 = ladder.resolve(87.0f);
  EXPECT_EQ(near12.fontId, 102);
  EXPECT_FLOAT_EQ(near12.residual, 1.0f);
  // 98% → body rung, residual 0.98 — snaps to plain unscaled body text.
  const auto nearBody = ladder.resolve(98.0f);
  EXPECT_EQ(nearBody.fontId, 0);
  EXPECT_FLOAT_EQ(nearBody.residual, 1.0f);
  // 80% → 12 pt rung, residual 0.941 — OUTSIDE the zone, stays scaled.
  const auto scaled = ladder.resolve(80.0f);
  EXPECT_EQ(scaled.fontId, 102);
  EXPECT_FLOAT_EQ(scaled.residual, 80.0f / 85.0f);
  // Empty ladder (SD fonts): the zone applies to the pure-scale residual too.
  const FontSizeLadder empty;
  EXPECT_FLOAT_EQ(empty.resolve(102.0f).residual, 1.0f);
  EXPECT_FLOAT_EQ(empty.resolve(95.0f).residual, 0.95f);
}

// A TTF family: every size its own face, at whole pixels per em.
namespace {
constexpr int32_t kNativeBase = 0x12345600;
constexpr uint32_t kBody14pt = (14 * 64 * 150 + 36) / 72;  // 29.17 px per em, 26.6

FontSizeLadder nativeLadder() {
  FontSizeLadder ladder;
  ladder.setNative(kNativeBase, kBody14pt, 10, 120);
  return ladder;
}
}  // namespace

TEST(FontSizeLadderTest, ANativeLadderGivesEachSizeItsOwnWholePixelFace) {
  const auto ladder = nativeLadder();
  // h1 160%: 46.7 px, drawn at 47 px; the 0.7% left over is inside the dead zone.
  const auto h1 = ladder.resolve(160.0f);
  EXPECT_EQ(kNativeBase | 47, h1.fontId);
  EXPECT_FLOAT_EQ(1.0f, h1.residual);
  const auto h2 = ladder.resolve(140.0f);
  EXPECT_EQ(kNativeBase | 41, h2.fontId);
  EXPECT_FLOAT_EQ(1.0f, h2.residual);
  // A footnote at 80%: 23.3 px at 23 px.
  const auto small = ladder.resolve(80.0f);
  EXPECT_EQ(kNativeBase | 23, small.fontId);
  EXPECT_FLOAT_EQ(1.0f, small.residual);
}

TEST(FontSizeLadderTest, ANativeLadderKeepsTheBodyFaceNearTheBodySize) {
  const auto ladder = nativeLadder();
  for (const float pct : {97.5f, 100.0f, 102.5f}) {
    const auto r = ladder.resolve(pct);
    EXPECT_EQ(0, r.fontId) << pct;
    EXPECT_FLOAT_EQ(1.0f, r.residual) << pct;
  }
  // 96%: 28.0 px is a face of its own, not the body's 29.17.
  EXPECT_EQ(kNativeBase | 28, ladder.resolve(96.0f).fontId);
}

TEST(FontSizeLadderTest, ANativeLadderScalesTheLargestFaceBeyondIt) {
  const auto ladder = nativeLadder();
  // A 5x drop cap: 145.8 px, the 120 px face scaled by the rest.
  const auto cap = ladder.resolve(500.0f);
  EXPECT_EQ(kNativeBase | 120, cap.fontId);
  EXPECT_NEAR(145.83f / 120.0f, cap.residual, 0.001f);
  // Tiny text: the smallest face, scaled down.
  const auto tiny = ladder.resolve(30.0f);
  EXPECT_EQ(kNativeBase | 10, tiny.fontId);
  EXPECT_NEAR(8.75f / 10.0f, tiny.residual, 0.001f);
}

TEST(FontSizeLadderTest, ANativeLadderClampsAbsurdSizesBeforeConvertingThem) {
  // A book's CSS can ask for any size (font-size: 1e12%); converting a float beyond int's range is undefined, and
  // differs between the host and the device. The largest face serves, scaled.
  const auto ladder = nativeLadder();
  EXPECT_EQ(kNativeBase | 120, ladder.resolve(1e12f).fontId);
  EXPECT_EQ(kNativeBase | 120, ladder.resolve(3e38f).fontId);
  EXPECT_EQ(kNativeBase | 10, ladder.resolve(1e-30f).fontId);
}

TEST(FontSizeLadderTest, AnIncompleteNativeLadderIsIgnored) {
  FontSizeLadder ladder;
  ladder.setNative(0, kBody14pt, 10, 120);
  ladder.setNative(kNativeBase, 0, 10, 120);
  ladder.setNative(kNativeBase, kBody14pt, 20, 10);
  EXPECT_FALSE(ladder.native());
  EXPECT_FLOAT_EQ(1.6f, ladder.resolve(160.0f).residual);  // the scale fallback
}

// ---------------------------------------------------------------------------
// ParsedText::foldUniformWordSizes — whole-paragraph spans join the block channel
// ---------------------------------------------------------------------------

TEST(WordSizeFolding, UniformNonDefaultFoldsIntoBlockMultiplier) {
  ParsedText text(false, false, noIndentStyle());
  for (const char* w : {"all", "words", "small"}) text.addWord(w, EpdFontFamily::REGULAR, false, false, 70);

  ASSERT_TRUE(text.foldUniformWordSizes());
  EXPECT_FLOAT_EQ(text.getBlockStyle().fontSizeMultiplier, 0.7f);

  // Per-word sizes were reset to 100 — the emitted line normalizes to the empty
  // (zero-cost) vector, and measurement now runs off the block multiplier.
  GfxRenderer renderer;
  const auto result = layout(text, renderer, 400);
  ASSERT_EQ(result.lines.size(), 1u);
  EXPECT_FALSE(result.lines[0]->hasWordSizes());
  EXPECT_FLOAT_EQ(result.lines[0]->getRenderStyle().fontSizeMultiplier, 0.7f);
}

TEST(WordSizeFolding, MixedOrDefaultSizesDoNotFold) {
  ParsedText mixed(false, false, noIndentStyle());
  mixed.addWord("normal", EpdFontFamily::REGULAR);
  mixed.addWord("small", EpdFontFamily::REGULAR, false, false, 70);
  EXPECT_FALSE(mixed.foldUniformWordSizes());
  EXPECT_FLOAT_EQ(mixed.getBlockStyle().fontSizeMultiplier, 1.0f);

  ParsedText uniform100(false, false, noIndentStyle());
  uniform100.addWord("plain", EpdFontFamily::REGULAR);
  EXPECT_FALSE(uniform100.foldUniformWordSizes());
}

TEST(WordSizeFolding, FoldComposesWithExistingBlockMultiplier) {
  BlockStyle bs = noIndentStyle();
  bs.fontSizeMultiplier = 1.4f;  // e.g. an h2 whose text sits in a 50% span
  ParsedText text(false, false, bs);
  text.addWord("word", EpdFontFamily::REGULAR, false, false, 50);

  ASSERT_TRUE(text.foldUniformWordSizes());
  EXPECT_FLOAT_EQ(text.getBlockStyle().fontSizeMultiplier, 0.7f);
}

// ---------------------------------------------------------------------------
// Render: baseline alignment of mixed-size words
// ---------------------------------------------------------------------------

TEST(WordSizeRender, MixedSizesBaselineAlign) {
  GfxRenderer renderer;
  // 200% word (ascender 32) next to 100% word (ascender 16): the smaller word's
  // glyph top must shift DOWN by the ascender difference so baselines meet.
  TextBlock line({"BIG", "small"}, {0, 70}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, BlockStyle(), {200, 100});
  line.render(renderer, kFontId, 0, 100);

  ASSERT_EQ(renderer.drawCalls.size(), 2u);
  const auto& big = renderer.drawCalls[0];
  const auto& small = renderer.drawCalls[1];
  EXPECT_FLOAT_EQ(big.scale, 2.0f);
  EXPECT_FLOAT_EQ(small.scale, 1.0f);
  EXPECT_EQ(big.y, 100);                                                          // tallest word sits at line top
  EXPECT_EQ(small.y, 100 + (2 * GfxRenderer::ASCENDER - GfxRenderer::ASCENDER));  // shifted to shared baseline
}

TEST(WordSizeRender, UniformLineRendersAtLineTop) {
  GfxRenderer renderer;
  TextBlock line({"a", "b"}, {0, 15}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, BlockStyle());
  line.render(renderer, kFontId, 0, 50);

  ASSERT_EQ(renderer.drawCalls.size(), 2u);
  EXPECT_EQ(renderer.drawCalls[0].y, 50);
  EXPECT_EQ(renderer.drawCalls[1].y, 50);
  EXPECT_FLOAT_EQ(renderer.drawCalls[0].scale, 1.0f);
}

// ---------------------------------------------------------------------------
// Interaction with the block-level multiplier (headings, block font-size)
// ---------------------------------------------------------------------------

// Sup/sub words carry an explicit size (parser default 50%) and the SUP/SUB bits shift
// the baseline relative to the BLOCK ascender. With ASCENDER=16: a 50% sup word on a
// 100% line baseline-aligns (+8) then raises by 16*2/5=6 → net +2 from line top. Its
// baseline lands at wordY + 16*0.5 = lineTop + 10, i.e. 6 px above the line baseline —
// exactly where the old hardwired-50% drawText path put it.
TEST(WordSizeRender, SupSubBaselineMatchesLegacyPositions) {
  GfxRenderer renderer;
  const auto sup = static_cast<EpdFontFamily::Style>(EpdFontFamily::REGULAR | EpdFontFamily::SUP);
  const auto sub = static_cast<EpdFontFamily::Style>(EpdFontFamily::REGULAR | EpdFontFamily::SUB);
  TextBlock line({"ref", "42", "x"}, {0, 40, 60}, {EpdFontFamily::REGULAR, sup, sub}, BlockStyle(), {100, 50, 50});
  line.render(renderer, kFontId, 0, 100);

  ASSERT_EQ(renderer.drawCalls.size(), 3u);
  EXPECT_EQ(renderer.drawCalls[0].y, 100);  // full-size word at line top
  EXPECT_FLOAT_EQ(renderer.drawCalls[1].scale, 0.5f);
  EXPECT_EQ(renderer.drawCalls[1].y, 100 + 8 - (16 * 2 / 5));  // baseline-align +8, SUP raise -6
  EXPECT_FLOAT_EQ(renderer.drawCalls[2].scale, 0.5f);
  EXPECT_EQ(renderer.drawCalls[2].y, 100 + 8 + (16 / 4));  // baseline-align +8, SUB lower +4
}

TEST(WordSizeRender, BlockMultiplierComposesWithWordSize) {
  GfxRenderer renderer;
  BlockStyle heading;
  heading.fontSizeMultiplier = 1.5f;
  TextBlock line({"word", "small"}, {0, 60}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, heading, {100, 50});
  line.render(renderer, kFontId, 0, 0);

  ASSERT_EQ(renderer.drawCalls.size(), 2u);
  EXPECT_FLOAT_EQ(renderer.drawCalls[0].scale, 1.5f);   // block multiplier alone
  EXPECT_FLOAT_EQ(renderer.drawCalls[1].scale, 0.75f);  // 1.5 x 50%
}

// ---------------------------------------------------------------------------
// Guide dots (render-time reading aid; idea from CrossInk)
// ---------------------------------------------------------------------------

// setGuideDots is a process-wide render option: always reset it so a failing
// test can't leak the enabled state into unrelated render tests.
class GuideDotsRender : public ::testing::Test {
 protected:
  void SetUp() override { TextBlock::setGuideDots(true); }
  void TearDown() override { TextBlock::setGuideDots(false); }
};

TEST_F(GuideDotsRender, DotCenteredInEachGapOnlyBetweenWords) {
  GfxRenderer renderer;
  // Fixed-width metrics: "aa"/"bb"/"cc" are 20 px wide, laid out with 5 px gaps.
  TextBlock line({"aa", "bb", "cc"}, {0, 25, 50},
                 {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, noIndentStyle());
  line.render(renderer, kFontId, 0, 100);

  // 3 words -> exactly 2 dots: none before the first word, none after the last.
  ASSERT_EQ(renderer.fillRectCalls.size(), 2u);
  // ASCENDER=16 -> dotSize 2. Gap [20,25): centered dot at 20 + (5-2)/2 = 21.
  EXPECT_EQ(renderer.fillRectCalls[0].x, 21);
  EXPECT_EQ(renderer.fillRectCalls[1].x, 46);
  // A third of the ascender above the baseline: 100 + 16 - 16/3 - 2/2 = 110.
  EXPECT_EQ(renderer.fillRectCalls[0].y, 110);
  EXPECT_EQ(renderer.fillRectCalls[0].w, 2);
  EXPECT_EQ(renderer.fillRectCalls[0].h, 2);
  EXPECT_TRUE(renderer.fillRectCalls[0].state);
}

TEST_F(GuideDotsRender, DisabledDrawsNoDots) {
  TextBlock::setGuideDots(false);
  GfxRenderer renderer;
  TextBlock line({"aa", "bb"}, {0, 25}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, noIndentStyle());
  line.render(renderer, kFontId, 0, 100);

  EXPECT_EQ(renderer.fillRectCalls.size(), 0u);
  EXPECT_EQ(renderer.drawCalls.size(), 2u);  // words still render
}

TEST_F(GuideDotsRender, CrampedGapGetsNoDot) {
  GfxRenderer renderer;
  // Adjacent styled runs of one word: "a" ends at x=10 and "b" starts there,
  // so there is no empty space to mark.
  const auto bold = static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD);
  TextBlock line({"a", "b"}, {0, 10}, {bold, EpdFontFamily::REGULAR}, noIndentStyle());
  line.render(renderer, kFontId, 0, 100);

  EXPECT_EQ(renderer.fillRectCalls.size(), 0u);
}

TEST_F(GuideDotsRender, SingleWordLineGetsNoDot) {
  GfxRenderer renderer;
  TextBlock line({"alone"}, {0}, {EpdFontFamily::REGULAR}, noIndentStyle());
  line.render(renderer, kFontId, 0, 100);

  EXPECT_EQ(renderer.fillRectCalls.size(), 0u);
}

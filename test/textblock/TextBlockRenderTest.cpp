// Coverage for TextBlock layout/render and its page-cache round-trip.
//
// TextBlock::render() is the per-line hot path: it runs for every line of every
// page render. It had no host coverage, so the ruby layout added in #2665 (group
// ruby, collision resolution, the baseline shift applied to ruby-bearing lines)
// was only checkable on device.
//
// These are characterisation tests. Expected pixel positions are derived from
// the synthetic metrics in stubs/GfxRenderer.h (GLYPH_W per codepoint, half that
// at SUP/SUB), so they pin down the exact geometry rather than merely asserting
// "something was drawn".

#include <gtest/gtest.h>

// Stub renderer (see stubs/GfxRenderer.h). TextBlock.h only forward-declares
// GfxRenderer via Block.h, so the test needs the definition explicitly.
#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "Epub/blocks/TextBlock.h"
#include "TestHelpers.h"

namespace {

constexpr int GW = GfxRenderer::GLYPH_W;
constexpr int ASC = GfxRenderer::ASCENDER;

// ---------------------------------------------------------------------------
// Ruby-less lines: the overwhelmingly common case.
// ---------------------------------------------------------------------------

TEST(TextBlockRender, RubylessLineDrawsWordsAtUnshiftedPositions) {
  auto block = makeBlock({"Hello", "world"}, {0, 60}, regularStyles(2));
  ASSERT_TRUE(block->valid());
  ASSERT_FALSE(block->hasRuby());

  const GfxRenderer renderer;
  block->render(renderer, 0, 100, 200);

  ASSERT_EQ(renderer.textCalls.size(), 2u);
  // No ruby on the line, so no baseline shift and no per-word x shift:
  // drawX == xpos + x, drawY == y.
  EXPECT_EQ(renderer.textCalls[0].text, "Hello");
  EXPECT_EQ(renderer.textCalls[0].x, 100);
  EXPECT_EQ(renderer.textCalls[0].y, 200);
  EXPECT_EQ(renderer.textCalls[1].text, "world");
  EXPECT_EQ(renderer.textCalls[1].x, 160);
  EXPECT_EQ(renderer.textCalls[1].y, 200);
}

TEST(TextBlockRender, RubylessLineEmitsNoSupCalls) {
  auto block = makeBlock({"plain", "text", "here"}, {0, 60, 110}, regularStyles(3));
  const GfxRenderer renderer;
  block->render(renderer, 0, 0, 50);

  ASSERT_EQ(renderer.textCalls.size(), 3u);
  for (const auto& call : renderer.textCalls) {
    EXPECT_EQ(call.style & EpdFontFamily::SUP, 0) << "ruby-less line must not emit SUP text: " << call.text;
    EXPECT_EQ(call.y, 50) << "ruby-less line must not shift the baseline";
  }
}

// A block whose rubyTexts vector is present but entirely empty must behave
// exactly like one with no ruby vector at all. This is the shape produced by
// deserializing a ruby-less block, since the cache format always stores one
// (empty) ruby string per word.
TEST(TextBlockRender, AllEmptyRubyVectorBehavesAsRubyless) {
  auto withoutRuby = makeBlock({"a", "bb"}, {0, 20}, regularStyles(2));
  auto withEmptyRuby = makeBlock({"a", "bb"}, {0, 20}, regularStyles(2), {"", ""});

  EXPECT_FALSE(withEmptyRuby->hasRuby());
  EXPECT_EQ(withEmptyRuby->getRubyShift(ASC), 0);

  const GfxRenderer r1;
  const GfxRenderer r2;
  withoutRuby->render(r1, 0, 10, 30);
  withEmptyRuby->render(r2, 0, 10, 30);

  expectSameTextCalls(r1, r2);
}

// ---------------------------------------------------------------------------
// Ruby lines.
// ---------------------------------------------------------------------------

TEST(TextBlockRender, RubyLineShiftsBaselineAndDrawsAnnotation) {
  // One ruby-annotated word. Base word is 2 codepoints wide (2 * GW = 20);
  // the annotation is 3 codepoints at half scale (3 * GW/2 = 15).
  auto block = makeBlock({"ab"}, {0}, regularStyles(1), {"xyz"});
  ASSERT_TRUE(block->hasRuby());
  EXPECT_EQ(block->getRubyShift(ASC), ASC / 2);

  const GfxRenderer renderer;
  block->render(renderer, 0, 0, 100);

  // Two draws: the base word, then its ruby annotation.
  ASSERT_EQ(renderer.textCalls.size(), 2u);

  const auto& base = renderer.textCalls[0];
  EXPECT_EQ(base.text, "ab");
  // Ruby-bearing lines shift the baseline down by ascender/2 to make room.
  EXPECT_EQ(base.y, 100 + ASC / 2);

  const auto& ruby = renderer.textCalls[1];
  EXPECT_EQ(ruby.text, "xyz");
  EXPECT_EQ(ruby.style & EpdFontFamily::SUP, EpdFontFamily::SUP);
  // Ruby sits one full ascender above the (already shifted) base baseline.
  EXPECT_EQ(ruby.y, base.y - ASC);
  // Annotation is narrower than the base word, so it is centred over it.
  const int baseW = renderer.getTextAdvanceX(0, "ab", EpdFontFamily::REGULAR);
  const int rubyW = renderer.getTextAdvanceX(0, "xyz", EpdFontFamily::SUP);
  EXPECT_EQ(ruby.x, base.x + (baseW - rubyW) / 2);
}

TEST(TextBlockRender, WideRubyRecentresBaseWord) {
  // Annotation wider than its base word: base word 1 codepoint (10), ruby 6
  // codepoints at half scale (30). The base word shifts right to stay centred
  // under the annotation, which is clamped to x >= 0 for the first word.
  auto block = makeBlock({"a"}, {0}, regularStyles(1), {"abcdef"});
  const GfxRenderer renderer;
  block->render(renderer, 0, 0, 100);

  ASSERT_EQ(renderer.textCalls.size(), 2u);
  const auto& base = renderer.textCalls[0];
  const auto& ruby = renderer.textCalls[1];

  EXPECT_EQ(ruby.text, "abcdef");
  EXPECT_EQ(ruby.x, 0) << "first word's ruby must not start left of the line origin";
  // Base word recentred under the wider annotation.
  const int baseW = renderer.getTextAdvanceX(0, "a", EpdFontFamily::REGULAR);
  const int rubyW = renderer.getTextAdvanceX(0, "abcdef", EpdFontFamily::SUP);
  EXPECT_EQ(base.x, ruby.x + (rubyW - baseW) / 2);
}

TEST(TextBlockRender, AdjacentRubyAnnotationsDoNotOverlap) {
  // Two adjacent annotated words, each 1 codepoint (10px) but with 4-codepoint
  // annotations (20px at half scale). Without collision resolution the two
  // annotations would overlap; the second must be pushed clear of the first.
  auto block = makeBlock({"a", "b"}, {0, 10}, regularStyles(2), {"wxyz", "wxyz"});
  const GfxRenderer renderer;
  block->render(renderer, 0, 0, 100);

  // 2 base words + 2 annotations.
  ASSERT_EQ(renderer.textCalls.size(), 4u);

  std::vector<const GfxRenderer::TextCall*> rubies;
  for (const auto& call : renderer.textCalls) {
    if ((call.style & EpdFontFamily::SUP) != 0) rubies.push_back(&call);
  }
  ASSERT_EQ(rubies.size(), 2u);

  const int firstEnd = rubies[0]->x + 4 * (GW / 2);
  EXPECT_GE(rubies[1]->x, firstEnd) << "second ruby annotation overlaps the first";
}

TEST(TextBlockRender, RubyContinueWordsGetNoOwnAnnotation) {
  // Group ruby: one annotation spanning two words, the second tagged
  // RUBY_CONTINUE. Only the group leader draws an annotation.
  auto block = makeBlock({"aa", "bb"}, {0, 20},
                         {EpdFontFamily::REGULAR, static_cast<Style>(EpdFontFamily::RUBY_CONTINUE)}, {"xy", ""});
  const GfxRenderer renderer;
  block->render(renderer, 0, 0, 100);

  EXPECT_EQ(countSupCalls(renderer), 1) << "a group-ruby run must draw exactly one annotation";
}

// ---------------------------------------------------------------------------
// Text decorations: exercised alongside ruby because both adjust the baseline.
// ---------------------------------------------------------------------------

TEST(TextBlockRender, UnderlineEmitsDecorationLine) {
  auto block = makeBlock({"underlined"}, {0}, {static_cast<Style>(EpdFontFamily::UNDERLINE)});
  const GfxRenderer renderer;
  block->render(renderer, 0, 0, 100);

  ASSERT_EQ(renderer.textCalls.size(), 1u);
  ASSERT_EQ(renderer.lineCalls.size(), 1u);
  EXPECT_EQ(renderer.lineCalls[0].y1, 100 + ASC + 2);
}

// ---------------------------------------------------------------------------
// Page-cache round-trip.
// ---------------------------------------------------------------------------

TEST(TextBlockSerialization, RoundTripPreservesWordsStylesAndPositions) {
  auto original = makeBlock({"first", "second", "third"}, {0, 55, 120},
                            {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, static_cast<Style>(EpdFontFamily::ITALIC)});

  HalFile file;
  ASSERT_TRUE(original->serialize(file));
  file.rewind();

  auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  ASSERT_TRUE(restored->valid());

  ASSERT_EQ(restored->wordCount(), 3);
  EXPECT_STREQ(restored->wordText(0), "first");
  EXPECT_STREQ(restored->wordText(1), "second");
  EXPECT_STREQ(restored->wordText(2), "third");
  EXPECT_EQ(restored->wordXpos(0), 0);
  EXPECT_EQ(restored->wordXpos(1), 55);
  EXPECT_EQ(restored->wordXpos(2), 120);
  EXPECT_EQ(restored->wordStyle(1), EpdFontFamily::BOLD);
  EXPECT_FALSE(restored->hasRuby());
}

TEST(TextBlockSerialization, RoundTripPreservesRuby) {
  auto original = makeBlock({"kanji", "plain"}, {0, 60}, regularStyles(2), {"kana", ""});

  HalFile file;
  ASSERT_TRUE(original->serialize(file));
  file.rewind();

  auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  ASSERT_TRUE(restored->hasRuby());
  ASSERT_EQ(restored->getRubyTexts().size(), 2u);
  EXPECT_EQ(restored->getRubyTexts()[0], "kana");
  EXPECT_TRUE(restored->getRubyTexts()[1].empty());
}

// A ruby-less block must survive the round trip still reporting no ruby, and
// must render identically before and after.
TEST(TextBlockSerialization, RubylessRoundTripRendersIdentically) {
  auto original = makeBlock({"alpha", "beta"}, {0, 60}, regularStyles(2));

  HalFile file;
  ASSERT_TRUE(original->serialize(file));
  file.rewind();
  auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);
  EXPECT_FALSE(restored->hasRuby());

  const GfxRenderer before;
  const GfxRenderer after;
  original->render(before, 0, 7, 21);
  restored->render(after, 0, 7, 21);

  expectSameTextCalls(before, after);
}

// The block's RTL flag feeds the bidi base direction passed to every drawText
// call, so losing it in the round trip would silently flip text direction.
//
// The words are digits deliberately. BidiUtils::detectParagraphLevel() only
// falls back to the block's RTL flag for direction-neutral text -- a word with a
// strong class (any Latin or Hebrew/Arabic letter) decides the direction by
// itself and the flag never gets consulted. So a Latin-text block round-trips
// identically whether or not the flag survives, which is why the ruby-less
// comparison above cannot catch a dropped isRtl.
TEST(TextBlockSerialization, RtlBlockRoundTripsBaseDirection) {
  BlockStyle rtl;
  rtl.isRtl = true;
  auto original = makeBlock({"123", "456"}, {0, 60}, regularStyles(2), {}, rtl);

  HalFile file;
  ASSERT_TRUE(original->serialize(file));
  file.rewind();
  auto restored = TextBlock::deserialize(file);
  ASSERT_NE(restored, nullptr);

  const GfxRenderer before;
  const GfxRenderer after;
  original->render(before, 0, 7, 21);
  restored->render(after, 0, 7, 21);

  ASSERT_FALSE(before.textCalls.empty());
  EXPECT_EQ(static_cast<int>(before.textCalls[0].baseDir), static_cast<int>(BidiUtils::BidiBaseDir::RTL))
      << "an RTL block must draw with an RTL base direction";
  expectSameTextCalls(before, after);
}

TEST(TextBlockSerialization, RejectsImplausibleWordCount) {
  // Corrupt header: word count beyond the 10000 cap must be refused rather than
  // driving a huge arena allocation.
  HalFile file;
  const uint16_t badWordCount = 60000;
  const uint8_t hasFocus = 0;
  const uint16_t textBytes = 8;
  file.write(&badWordCount, sizeof(badWordCount));
  file.write(&hasFocus, sizeof(hasFocus));
  file.write(&textBytes, sizeof(textBytes));
  file.rewind();

  EXPECT_EQ(TextBlock::deserialize(file), nullptr);
}

}  // namespace

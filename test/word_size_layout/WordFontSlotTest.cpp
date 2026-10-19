// WordFontSlotTest.cpp
//
// Per-word font slots (Publisher Fonts: a word in another of the book's families than its block's,
// TextBlock::WordFont), on the real ParsedText and TextBlock against the fixed-width renderer shim, whose
// WIDE_FONT stands for the other family:
//
//   - a word in a slot is measured, spaced and broken in its slot's font, at the slot's scale
//   - layout asks each font to get ready for its own words only
//   - a line carries the fonts its words use, and a line without is the bytes it was before
//   - a split word keeps its slot on both halves
//   - render and wordBox draw and box the word in its font, on the line's baseline
//   - serialization keeps the fonts, and refuses a slot that names none
//
// Per-word font slots follow crosspoint-reader PR #3934 (Justin Mitchell).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;
constexpr int kWide = GfxRenderer::WIDE_FONT;
constexpr auto kRegular = EpdFontFamily::REGULAR;

BlockStyle noIndentStyle() {
  BlockStyle bs;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  return bs;
}

std::vector<std::unique_ptr<TextBlock>> layout(ParsedText& text, const GfxRenderer& renderer, const uint16_t width) {
  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, width,
      [&lines](std::unique_ptr<TextBlock> line, bool, bool) {
        lines.push_back(std::move(line));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  return lines;
}

std::string serialized(const TextBlock& block) {
  FsFile file = HalFile::forReadWrite();
  EXPECT_TRUE(block.serialize(file));
  EXPECT_TRUE(file.seek(0));
  std::string bytes(file.size(), '\0');
  file.read(bytes.data(), bytes.size());
  return bytes;
}

// "aa bb cc", bb in slot 1.
ParsedText threeWords(const TextBlock::WordFont& slot1) {
  ParsedText text(false, false, noIndentStyle());
  text.addWord("aa", kRegular);
  text.addWord("bb", kRegular, false, false, 100, 0, /*fontSlot=*/1);
  text.addWord("cc", kRegular);
  text.setFontSlot(1, slot1);
  return text;
}

}  // namespace

TEST(WordFontSlots, AWordInASlotIsMeasuredAndSpacedInItsFont) {
  GfxRenderer renderer;
  ParsedText text = threeWords({kWide, 1.0f});
  const auto lines = layout(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  EXPECT_EQ(0, line.wordXpos(0));
  EXPECT_EQ(20 + GfxRenderer::SPACE_W, line.wordXpos(1));  // aa, then aa's space
  // bb is two wide glyphs, and the space after it is bb's font's.
  EXPECT_EQ(line.wordXpos(1) + 2 * GfxRenderer::WIDE_GLYPH_W + GfxRenderer::WIDE_SPACE_W, line.wordXpos(2));
  EXPECT_EQ(nullptr, line.wordFont(0));
  ASSERT_NE(nullptr, line.wordFont(1));
  EXPECT_EQ(kWide, line.wordFont(1)->fontId);
  EXPECT_EQ(nullptr, line.wordFont(2));
}

TEST(WordFontSlots, TheSlotsScaleStandsInForTheBlocksAndComposesWithTheWordsSize) {
  GfxRenderer renderer;
  BlockStyle style = noIndentStyle();
  style.fontSizeMultiplier = 2.0f;  // the block's residual: not the word's
  ParsedText text(false, false, style);
  text.addWord("aa", kRegular);
  text.addWord("bb", kRegular, false, false, /*sizePct=*/50, 0, /*fontSlot=*/1);
  text.setFontSlot(1, {kWide, 1.6f});
  const auto lines = layout(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  renderer.drawCalls.clear();
  lines[0]->render(renderer, kFontId, 0, 0);
  ASSERT_EQ(2u, renderer.drawCalls.size());
  EXPECT_EQ(kFontId, renderer.drawCalls[0].fontId);
  EXPECT_FLOAT_EQ(2.0f, renderer.drawCalls[0].scale);
  EXPECT_EQ(kWide, renderer.drawCalls[1].fontId);
  EXPECT_FLOAT_EQ(0.8f, renderer.drawCalls[1].scale);  // 1.6 x 50 %
}

TEST(WordFontSlots, EachFontGetsReadyForItsOwnWords) {
  GfxRenderer renderer;
  ParsedText text = threeWords({kWide, 1.0f});
  layout(text, renderer, 400);
  ASSERT_EQ(2u, renderer.readyCalls.size());
  EXPECT_EQ(kFontId, renderer.readyCalls[0].fontId);
  EXPECT_EQ("aa cc-", renderer.readyCalls[0].text);
  EXPECT_EQ(kWide, renderer.readyCalls[1].fontId);
  EXPECT_EQ("bb-", renderer.readyCalls[1].text);
}

TEST(WordFontSlots, AnEmptySlotIsTheBlocksFontAndCostsNothing) {
  GfxRenderer renderer;
  ParsedText slotted = threeWords({});
  ParsedText plain(false, false, noIndentStyle());
  for (const char* word : {"aa", "bb", "cc"}) plain.addWord(word, kRegular);
  const auto a = layout(slotted, renderer, 400);
  const auto b = layout(plain, renderer, 400);
  ASSERT_EQ(1u, a.size());
  ASSERT_EQ(1u, b.size());
  EXPECT_EQ(nullptr, a[0]->wordFont(1));
  EXPECT_EQ(serialized(*b[0]), serialized(*a[0]));
}

TEST(WordFontSlots, ALineCarriesOnlyTheFontsOfItsOwnWords) {
  GfxRenderer renderer;
  // Two lines of 50 px: "aa bb" and "cc dd", dd in the wide font.
  ParsedText text(false, false, noIndentStyle());
  ParsedText plain(false, false, noIndentStyle());
  for (const char* word : {"aa", "bb", "cc"}) {
    text.addWord(word, kRegular);
    plain.addWord(word, kRegular);
  }
  text.addWord("d", kRegular, false, false, 100, 0, /*fontSlot=*/2);
  plain.addWord("d", kRegular);
  text.setFontSlot(2, {kWide, 1.0f});
  const auto lines = layout(text, renderer, 50);
  const auto plainLines = layout(plain, renderer, 50);
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(2u, plainLines.size());
  EXPECT_EQ(serialized(*plainLines[0]), serialized(*lines[0]));  // byte-identical without word fonts
  const TextBlock& second = *lines[1];
  ASSERT_EQ(2u, second.wordCount());
  EXPECT_EQ(nullptr, second.wordFont(0));
  ASSERT_NE(nullptr, second.wordFont(1));
  EXPECT_EQ(kWide, second.wordFont(1)->fontId);
}

TEST(WordFontSlots, ASplitWordKeepsItsSlotOnBothHalves) {
  GfxRenderer renderer;
  // Ten glyphs fit 100 px in the block's font; in the wide font (150 px) the word is split to fit.
  ParsedText text(false, false, noIndentStyle());
  text.addWord("abcdefghij", kRegular, false, false, 100, 0, /*fontSlot=*/1);
  text.setFontSlot(1, {kWide, 1.0f});
  const auto lines = layout(text, renderer, 100);
  ASSERT_GE(lines.size(), 2u);
  for (const auto& line : lines) {
    for (uint16_t w = 0; w < line->wordCount(); ++w) {
      ASSERT_NE(nullptr, line->wordFont(w)) << line->wordText(w);
      EXPECT_EQ(kWide, line->wordFont(w)->fontId);
    }
  }
}

TEST(WordFontSlots, RenderAndWordBoxUseTheWordsFontOnTheLinesBaseline) {
  GfxRenderer renderer;
  ParsedText text = threeWords({kWide, 1.0f});
  const auto lines = layout(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  renderer.drawCalls.clear();
  line.render(renderer, kFontId, 0, 100);
  ASSERT_EQ(3u, renderer.drawCalls.size());
  EXPECT_EQ(kFontId, renderer.drawCalls[0].fontId);
  EXPECT_EQ(100, renderer.drawCalls[0].y);
  EXPECT_EQ(kWide, renderer.drawCalls[1].fontId);
  // The line keeps its font's baseline (100 + 16): the taller wide font's top sits above the line's.
  EXPECT_EQ(100 + GfxRenderer::ASCENDER - GfxRenderer::WIDE_ASCENDER, renderer.drawCalls[1].y);

  const TextBlock::WordBox box = line.wordBox(renderer, 1, kFontId, 0, 100);
  EXPECT_EQ(kWide, box.fontId);
  EXPECT_EQ(renderer.drawCalls[1].x, box.x);
  EXPECT_EQ(renderer.drawCalls[1].y, box.y);
  EXPECT_EQ(2 * GfxRenderer::WIDE_GLYPH_W, box.width);
}

TEST(WordFontSlots, SerializationKeepsTheFontsAndRefusesASlotThatNamesNone) {
  GfxRenderer renderer;
  ParsedText text = threeWords({kWide, 1.25f});
  const auto lines = layout(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const std::string bytes = serialized(*lines[0]);

  FsFile file = HalFile::forReadWrite();
  file.write(bytes.data(), bytes.size());
  ASSERT_TRUE(file.seek(0));
  const auto restored = TextBlock::deserialize(file);
  ASSERT_NE(nullptr, restored);
  EXPECT_EQ(nullptr, restored->wordFont(0));
  ASSERT_NE(nullptr, restored->wordFont(1));
  EXPECT_EQ(kWide, restored->wordFont(1)->fontId);
  EXPECT_FLOAT_EQ(1.25f, restored->wordFont(1)->scale);
  EXPECT_EQ("bb", std::string(restored->wordText(1)));

  // Header (u16 words, u8 flags, u16 text bytes), one 8-byte font, then textOff, xpos, styles and the slots.
  constexpr size_t kSlots = 5 + 8 + 3 * 2 + 3 * 2 + 3;
  std::string corrupt = bytes;
  ASSERT_EQ(1, corrupt[kSlots + 1]);
  corrupt[kSlots + 1] = 2;  // the line has one font
  FsFile bad = HalFile::forReadWrite();
  bad.write(corrupt.data(), corrupt.size());
  ASSERT_TRUE(bad.seek(0));
  EXPECT_EQ(nullptr, TextBlock::deserialize(bad));
}

#include <GfxRenderer.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lib/Epub/Epub/ParsedText.h"
#include "lib/Epub/Epub/blocks/TextBlock.h"
#include "test/test_harness.h"

// ===== Test: basic word addition and layout =====

void testBasicLayout() {
  GfxRenderer renderer;
  // With stub: each char = 8px wide, space = 5px, line height = 20px
  // Viewport width = 200px

  ParsedText text(false);  // no extra paragraph spacing
  text.addWord("Hello", EpdFontFamily::REGULAR);
  text.addWord("world", EpdFontFamily::REGULAR);

  ASSERT_EQ(text.size(), 2u);
  ASSERT_FALSE(text.isEmpty());

  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(renderer, 0, 200,
                             [&lines](std::unique_ptr<TextBlock> block) { lines.push_back(std::move(block)); });

  // "Hello" = 5*8=40, "world" = 5*8=40, space=5 => 85px total. Fits in 200px => 1 line
  ASSERT_EQ(lines.size(), 1u);
  ASSERT_FALSE(lines[0]->isEmpty());
}

// ===== Test: line wrapping =====

void testLineWrapping() {
  GfxRenderer renderer;
  // Viewport = 100px. "Hello" = 40px, "world" = 40px, space = 5px => 85 < 100, fits.
  // Add more words to force wrapping.

  ParsedText text(false);
  text.addWord("Hello", EpdFontFamily::REGULAR);
  text.addWord("world", EpdFontFamily::REGULAR);
  text.addWord("this", EpdFontFamily::REGULAR);  // 4*8=32
  text.addWord("is", EpdFontFamily::REGULAR);    // 2*8=16
  text.addWord("a", EpdFontFamily::REGULAR);     // 1*8=8
  text.addWord("test", EpdFontFamily::REGULAR);  // 4*8=32

  // Total if on one line: 40+5+40+5+32+5+16+5+8+5+32 = 193px
  // With viewport=100, should wrap to multiple lines

  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(renderer, 0, 100,
                             [&lines](std::unique_ptr<TextBlock> block) { lines.push_back(std::move(block)); });

  ASSERT_TRUE(lines.size() >= 2);  // Must wrap to at least 2 lines
}

// ===== Test: continuation words (attachToPrevious) =====

void testContinuationWords() {
  GfxRenderer renderer;

  ParsedText text(false);
  text.addWord("Hello", EpdFontFamily::REGULAR);
  text.addWord(",", EpdFontFamily::REGULAR, false, true);  // attaches to "Hello"
  text.addWord("world", EpdFontFamily::REGULAR);

  ASSERT_EQ(text.size(), 3u);

  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(renderer, 0, 400,
                             [&lines](std::unique_ptr<TextBlock> block) { lines.push_back(std::move(block)); });

  // All fits on one line
  ASSERT_EQ(lines.size(), 1u);
}

// ===== Test: single oversized word =====

void testSingleOversizedWord() {
  GfxRenderer renderer;

  ParsedText text(false);
  // A very long word: 30 chars * 8px = 240px, viewport=100px
  text.addWord("abcdefghijklmnopqrstuvwxyzabcd", EpdFontFamily::REGULAR);

  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(renderer, 0, 100,
                             [&lines](std::unique_ptr<TextBlock> block) { lines.push_back(std::move(block)); });

  // Should produce at least one line (forced onto its own line)
  ASSERT_TRUE(lines.size() >= 1);
}

// ===== Test: empty text =====

void testEmptyText() {
  GfxRenderer renderer;

  ParsedText text(false);
  ASSERT_TRUE(text.isEmpty());
  ASSERT_EQ(text.size(), 0u);

  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(renderer, 0, 200,
                             [&lines](std::unique_ptr<TextBlock> block) { lines.push_back(std::move(block)); });

  ASSERT_EQ(lines.size(), 0u);
}

// ===== Test: TextBlock serialize/deserialize round-trip =====

void testTextBlockSerializeRoundTrip() {
  std::vector<std::string> words = {"Hello", "world"};
  std::vector<uint16_t> xpos = {0, 45};
  std::vector<EpdFontFamily::Style> styles = {EpdFontFamily::REGULAR, EpdFontFamily::BOLD};

  BlockStyle bs;
  bs.alignment = CssTextAlign::Justify;

  auto block = std::make_unique<TextBlock>(words, xpos, styles, bs);
  ASSERT_FALSE(block->isEmpty());

  // Serialize to FsFile (in-memory buffer)
  FsFile file;
  auto buf = std::make_shared<std::vector<uint8_t>>();
  file.initBuffer(buf);

  ASSERT_TRUE(block->serialize(file));

  // Deserialize
  FsFile readFile;
  readFile.initBuffer(buf);

  auto restored = TextBlock::deserialize(readFile);
  ASSERT_TRUE(restored != nullptr);
  ASSERT_FALSE(restored->isEmpty());
}

// ===== Test: bold/italic styles preserved through layout =====

void testStylesPreserved() {
  GfxRenderer renderer;

  ParsedText text(false);
  text.addWord("Bold", EpdFontFamily::BOLD);
  text.addWord("Normal", EpdFontFamily::REGULAR);
  text.addWord("Italic", EpdFontFamily::ITALIC);

  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(renderer, 0, 400,
                             [&lines](std::unique_ptr<TextBlock> block) { lines.push_back(std::move(block)); });

  ASSERT_EQ(lines.size(), 1u);
}

// ===== Test: includeLastLine=false drops last line =====

void testExcludeLastLine() {
  GfxRenderer renderer;

  ParsedText text(false);
  // Force multiple lines: viewport=50, "Hello"=40, "world"=40
  text.addWord("Hello", EpdFontFamily::REGULAR);
  text.addWord("world", EpdFontFamily::REGULAR);

  std::vector<std::unique_ptr<TextBlock>> linesAll;
  ParsedText text2(false);
  text2.addWord("Hello", EpdFontFamily::REGULAR);
  text2.addWord("world", EpdFontFamily::REGULAR);

  text2.layoutAndExtractLines(renderer, 0, 50,
                              [&linesAll](std::unique_ptr<TextBlock> block) { linesAll.push_back(std::move(block)); });

  std::vector<std::unique_ptr<TextBlock>> linesPartial;
  text.layoutAndExtractLines(
      renderer, 0, 50, [&linesPartial](std::unique_ptr<TextBlock> block) { linesPartial.push_back(std::move(block)); },
      false);

  // With includeLastLine=false, should have one fewer line
  if (linesAll.size() > 1) {
    ASSERT_EQ(linesPartial.size(), linesAll.size() - 1);
  } else {
    ASSERT_EQ(linesPartial.size(), 0u);
  }
}

// ===== Test: justified alignment =====

void testJustifiedAlignment() {
  GfxRenderer renderer;

  BlockStyle bs;
  bs.alignment = CssTextAlign::Justify;
  bs.textAlignDefined = true;

  ParsedText text(false, false, bs);
  // Make words that definitely wrap: viewport=80
  // "aa" = 16, "bb" = 16, "cc" = 16 => 16+5+16+5+16 = 58 < 80, fits on one line
  // "dd" = 16 => 58+5+16 = 79 < 80, still fits
  // "ee" = 16 => 79+5+16 = 100 > 80, wraps
  text.addWord("aa", EpdFontFamily::REGULAR);
  text.addWord("bb", EpdFontFamily::REGULAR);
  text.addWord("cc", EpdFontFamily::REGULAR);
  text.addWord("dd", EpdFontFamily::REGULAR);
  text.addWord("ee", EpdFontFamily::REGULAR);

  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(renderer, 0, 80,
                             [&lines](std::unique_ptr<TextBlock> block) { lines.push_back(std::move(block)); });

  ASSERT_TRUE(lines.size() >= 2);
}

int main() {
  std::cout << "TextLayoutTest\n";
  RUN_TEST(testBasicLayout);
  RUN_TEST(testLineWrapping);
  RUN_TEST(testContinuationWords);
  RUN_TEST(testSingleOversizedWord);
  RUN_TEST(testEmptyText);
  RUN_TEST(testTextBlockSerializeRoundTrip);
  RUN_TEST(testStylesPreserved);
  RUN_TEST(testExcludeLastLine);
  RUN_TEST(testJustifiedAlignment);
  TEST_SUMMARY();
}

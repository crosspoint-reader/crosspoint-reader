#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "blocks/TextBlock.h"

namespace {

std::string tempPath(const char* name) { return testing::TempDir() + name; }

std::unique_ptr<TextBlock> roundTrip(const TextBlock& block, const char* name) {
  const std::string path = tempPath(name);
  {
    HalFile out;
    EXPECT_TRUE(out.open(path.c_str(), "wb"));
    EXPECT_TRUE(block.serialize(out));
  }
  HalFile in;
  EXPECT_TRUE(in.open(path.c_str(), "rb"));
  auto restored = TextBlock::deserialize(in);
  std::remove(path.c_str());
  return restored;
}

// A shaped word as the renderer stores it: arbitrary bytes that are not the logical text.
const std::string kShaped = "\xF3\xBE\x80\x80\xF3\xB0\x80\x96";

}  // namespace

TEST(TextBlockDisplay, KeepsLogicalAndDisplayTextApart) {
  const std::vector<std::string> words = {"hello", "\xE0\xA6\x95\xE0\xA6\xBF", "world"};  // hello কি world
  const std::vector<std::string> display = {"", kShaped, ""};
  TextBlock block(words, {0, 50, 90}, {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::REGULAR}, {}, {},
                  BlockStyle(), {}, {}, display);
  ASSERT_TRUE(block.valid());
  EXPECT_STREQ(block.wordText(1), words[1].c_str());
  EXPECT_EQ(block.displayText(1), kShaped);
  EXPECT_STREQ(block.displayText(0), "hello");  // no display form: draws the text itself
  EXPECT_STREQ(block.displayText(2), "world");

  const auto restored = roundTrip(block, "display.bin");
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->wordCount(), 3);
  for (uint16_t i = 0; i < 3; i++) {
    EXPECT_STREQ(restored->wordText(i), words[i].c_str());
    EXPECT_STREQ(restored->displayText(i), block.displayText(i));
    EXPECT_EQ(restored->wordXpos(i), block.wordXpos(i));
    EXPECT_EQ(restored->wordStyle(i), block.wordStyle(i));
  }
}

TEST(TextBlockDisplay, LinesWithoutComplexScriptStoreNoDisplayRegion) {
  const std::vector<std::string> words = {"plain", "line"};
  TextBlock block(words, {0, 40}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, {}, {});
  ASSERT_TRUE(block.valid());
  const auto restored = roundTrip(block, "plain.bin");
  ASSERT_NE(restored, nullptr);
  EXPECT_STREQ(restored->displayText(0), "plain");
  EXPECT_STREQ(restored->displayText(1), "line");
}

TEST(TextBlockDisplay, RejectsACorruptDisplayOffset) {
  const std::vector<std::string> words = {"\xE0\xA6\x95", "\xE0\xA6\x96"};
  TextBlock block(words, {0, 20}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, {}, {}, BlockStyle(), {}, {},
                  {kShaped, kShaped});
  const std::string path = tempPath("corrupt.bin");
  {
    HalFile out;
    ASSERT_TRUE(out.open(path.c_str(), "wb"));
    ASSERT_TRUE(block.serialize(out));
  }
  // Header: wordCount(2) flags(1) textBytes(2) displayBytes(2); then textOff[2], xpos[2], displayOff[2].
  std::FILE* f = std::fopen(path.c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  const long displayOff1 = 7 + 2 * 2 + 2 * 2 + 2;
  std::fseek(f, displayOff1, SEEK_SET);
  const uint8_t bad[2] = {3, 0};  // points into the middle of the first entry
  std::fwrite(bad, 1, 2, f);
  std::fclose(f);

  HalFile in;
  ASSERT_TRUE(in.open(path.c_str(), "rb"));
  EXPECT_EQ(TextBlock::deserialize(in), nullptr);
  std::remove(path.c_str());
}

TEST(TextBlockDisplay, ComplexScriptLineWithNothingToRedrawRoundTrips) {
  // An Indic line whose words all draw as their own text (unshaped, with no
  // vowel sign to reorder): the display vector exists but every entry is empty.
  const std::vector<std::string> words = {"\xE0\xA4\x95\xE0\xA5\x80", "\xE0\xA4\xB9\xE0\xA5\x88"};  // की है
  TextBlock block(words, {0, 30}, {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, {}, {}, BlockStyle(), {}, {},
                  {"", ""});
  ASSERT_TRUE(block.valid());
  const auto restored = roundTrip(block, "empty-display.bin");
  ASSERT_NE(restored, nullptr) << "a line with no display forms must not store an empty display region";
  EXPECT_STREQ(restored->displayText(0), words[0].c_str());
  EXPECT_STREQ(restored->displayText(1), words[1].c_str());
}

TEST(TextBlockDisplay, FocusSplitWordsDrawTheirLogicalText) {
  // A corrupt cache can pair a focus boundary with a display form shorter than
  // the boundary; the split must still index the logical text.
  const std::vector<std::string> words = {"abcdef"};
  TextBlock block(words, {0}, {EpdFontFamily::REGULAR}, {3}, {24}, BlockStyle(), {}, {}, {"x"});
  ASSERT_TRUE(block.valid());
  GfxRenderer renderer;
  block.render(renderer, 0, 0, 0);
  ASSERT_EQ(renderer.drawnTexts.size(), 2u);
  EXPECT_EQ(renderer.drawnTexts[0], "abc");
  EXPECT_EQ(renderer.drawnTexts[1], "def");
}

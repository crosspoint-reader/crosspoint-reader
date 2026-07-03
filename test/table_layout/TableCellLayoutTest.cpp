// Regression tests for table cell text layout in narrow columns.
//
// A table column can be far narrower than a single header word. The layout must
// break such words to fit — falling back to character-level breaking when the
// language's hyphenation patterns offer no break point narrow enough — so a word
// NEVER overflows its column. This reproduces the field bug where "Probability"
// and "Average" spilled across column borders while "Management"/"Standard"
// happened to have a short-enough pattern break.
//
// The test drives the real ParsedText line-breaker with the real English
// hyphenation patterns; only GfxRenderer is mocked (uniform glyph width) so the
// arithmetic is deterministic.
#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lib/Epub/Epub/ParsedText.h"
#include "lib/Epub/Epub/blocks/BlockStyle.h"
#include "lib/Epub/Epub/blocks/TextBlock.h"
#include "lib/Epub/Epub/hyphenation/Hyphenator.h"

// ---- Mock GfxRenderer: every codepoint is kGlyphWidth px wide ----
namespace {
constexpr int kGlyphWidth = 8;
}

HWCDC Serial;

int GfxRenderer::getLineHeight(int) const { return 20; }
int GfxRenderer::getFontAscenderSize(int) const { return 12; }
int GfxRenderer::getSpaceWidth(int, EpdFontFamily::Style) const { return kGlyphWidth; }
int GfxRenderer::getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
  int cps = 0;
  for (const char* p = text; *p; ++p) {
    if ((*p & 0xC0) != 0x80) ++cps;  // count UTF-8 lead bytes
  }
  return cps * kGlyphWidth;
}
int GfxRenderer::getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return kGlyphWidth; }
int GfxRenderer::getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
void GfxRenderer::ensureSdCardFontReady(int, const std::vector<std::string>&, bool, uint8_t) const {}
void GfxRenderer::ensureSdCardFontReady(int, const char*, uint8_t) const {}

namespace {

// A zero-initialized GfxRenderer is enough: the only real member touched is the
// empty SD-card-font map inside isSdCardFont(), which reports "not an SD font".
struct MockRenderer {
  std::vector<char> storage{std::vector<char>(sizeof(GfxRenderer) + 64, 0)};
  GfxRenderer& get() { return *reinterpret_cast<GfxRenderer*>(storage.data()); }
};

std::unique_ptr<ParsedText> makeCell(const std::vector<std::string>& words, bool hyphenation) {
  BlockStyle style;
  style.textAlignDefined = true;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  style.textIndent = 0;
  auto text = std::make_unique<ParsedText>(/*extraParagraphSpacing=*/false, hyphenation,
                                           /*focusReadingEnabled=*/false, style);
  for (const auto& w : words) {
    text->addWord(w, EpdFontFamily::REGULAR);
  }
  return text;
}

// Widest single laid-out token, in px. If any token is wider than the column,
// the layout let a word overflow.
int widestToken(ParsedText& cell, GfxRenderer& renderer, int columnWidth) {
  int worst = 0;
  cell.layoutAndExtractLines(
      renderer, /*fontId=*/0, static_cast<uint16_t>(columnWidth), [&](const std::shared_ptr<TextBlock>& line) {
        for (const auto& w : line->getWords()) {
          worst = std::max(worst, renderer.getTextAdvanceX(0, w.c_str(), EpdFontFamily::REGULAR));
        }
      });
  return worst;
}

// The exact header labels from the field report (Table 6.1, SPY strangles).
const std::vector<std::vector<std::string>> kReproHeaders = {
    {"Management", "DTE"},     {"Probability", "of", "Profit", "(POP)"}, {"Average", "P/L"}, {"Daily", "P/L"},
    {"Standard", "Deviation"}, {"Conditional", "Value", "at", "Risk"},
};

class TableCellLayoutTest : public ::testing::Test {
 protected:
  void SetUp() override { Hyphenator::setPreferredLanguage("en"); }
  MockRenderer mock;
};

// No header word may overflow a narrow column, with hyphenation enabled.
TEST_F(TableCellLayoutTest, NarrowColumnNeverOverflowsHyphenated) {
  constexpr int kColumnWidth = 32;  // 4 glyphs — narrower than the shortest pattern prefix
  for (const auto& header : kReproHeaders) {
    auto cell = makeCell(header, /*hyphenation=*/true);
    const int worst = widestToken(*cell, mock.get(), kColumnWidth);
    EXPECT_LE(worst, kColumnWidth) << "overflow laying out: " << header.front();
  }
}

// Same guarantee with hyphenation disabled (the non-hyphenated pre-split path).
TEST_F(TableCellLayoutTest, NarrowColumnNeverOverflowsUnhyphenated) {
  constexpr int kColumnWidth = 32;
  for (const auto& header : kReproHeaders) {
    auto cell = makeCell(header, /*hyphenation=*/false);
    const int worst = widestToken(*cell, mock.get(), kColumnWidth);
    EXPECT_LE(worst, kColumnWidth) << "overflow laying out: " << header.front();
  }
}

// The specific words from the bug ("Probability", "Average") must break even at
// widths where their earliest English pattern break is still too wide.
TEST_F(TableCellLayoutTest, PatternResistantWordsStillBreak) {
  for (const char* word : {"Probability", "Average", "Standard", "Conditional"}) {
    for (int columnWidth : {16, 24, 32, 40}) {
      auto cell = makeCell({word}, /*hyphenation=*/true);
      const int worst = widestToken(*cell, mock.get(), columnWidth);
      EXPECT_LE(worst, columnWidth) << word << " overflowed column width " << columnWidth;
    }
  }
}

// A comfortably wide column must NOT introduce spurious mid-word breaks: the
// character fallback only engages when nothing else fits.
TEST_F(TableCellLayoutTest, WideColumnKeepsWordsWhole) {
  constexpr int kWideColumn = 400;
  auto cell = makeCell({"Probability", "Average", "Deviation"}, /*hyphenation=*/true);
  size_t lines = 0;
  cell->layoutAndExtractLines(mock.get(), 0, kWideColumn, [&](const std::shared_ptr<TextBlock>& line) {
    ++lines;
    for (const auto& w : line->getWords()) {
      EXPECT_EQ(w.find('-'), std::string::npos) << "spurious hyphen in wide column: " << w;
    }
  });
  EXPECT_EQ(lines, 1u);
}

}  // namespace

#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <MiniBidi/BidiUtils.h>
#include <gtest/gtest.h>

#include <algorithm>

TEST(FocusReading, PreservesLetterOrderAndRunPlacement) {
  for (const bool rtlParagraph : {false, true}) {
    for (const int8_t tracking : {0, 2}) {
      for (const bool focus : {false, true}) {
        for (const auto style : {EpdFontFamily::REGULAR, EpdFontFamily::ITALIC, EpdFontFamily::BOLD}) {
          for (const char* word : {"שלום", "בראשית", "התלמידים", "reading", "١٢٣٤"}) {
            SCOPED_TRACE(word);
            GfxRenderer renderer;
            renderer.boldGlyphAdvance = 11;
            BlockStyle blockStyle;
            blockStyle.isRtl = rtlParagraph;
            blockStyle.directionDefined = true;
            blockStyle.characterSpacing = tracking;
            ParsedText text(false, focus, blockStyle, 0);
            text.addWord(word, style);
            text.layoutAndExtractLines(
                renderer, 0, 400, [&](auto block, auto) { block->render(renderer, 0, 0, 0); }, true, tracking);
            ASSERT_FALSE(renderer.textDraws.empty());
            std::sort(renderer.textDraws.begin(), renderer.textDraws.end(),
                      [](const auto& a, const auto& b) { return a.x < b.x; });
            std::string actual;
            int nextX = renderer.textDraws.front().x;
            for (const auto& draw : renderer.textDraws) {
              EXPECT_EQ(draw.x, nextX);
              nextX += renderer.getTextAdvanceX(0, draw.text.c_str(), draw.style, tracking) + tracking;
              std::string visual;
              ASSERT_TRUE(BidiUtils::applyBidiVisual(draw.text.c_str(), visual, static_cast<int>(draw.direction)));
              actual += visual;
            }
            std::string expected;
            ASSERT_TRUE(BidiUtils::applyBidiVisual(word, expected));
            EXPECT_EQ(actual, expected);
            if (focus && style != EpdFontFamily::BOLD) {
              ASSERT_EQ(renderer.textDraws.size(), 2u);
              const bool rtlWord = BidiUtils::startsWithRtl(word);
              EXPECT_TRUE(renderer.textDraws[rtlWord ? 1 : 0].style & EpdFontFamily::BOLD);
              EXPECT_FALSE(renderer.textDraws[rtlWord ? 0 : 1].style & EpdFontFamily::BOLD);
              if (style == EpdFontFamily::ITALIC) {
                for (const auto& draw : renderer.textDraws) EXPECT_TRUE(draw.style & EpdFontFamily::ITALIC);
              }
            }
          }
        }
      }
    }
  }
}
